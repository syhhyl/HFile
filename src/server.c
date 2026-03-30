#include "cli.h"
#include "http.h"
#include "message_store.h"
#include "net.h"
#include "protocol.h"
#include "shutdown.h"
#include "server.h"
#include "transfer_io.h"

#include <stddef.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"

#ifdef _WIN32
  #include <io.h>
  #include <process.h>
#else
  #include <pthread.h>
  #include <unistd.h>
#endif

typedef struct {
  socket_t sock;
  server_opt_t opt;
} http_thread_ctx_t;

typedef struct {
  int started;
#ifdef _WIN32
  HANDLE handle;
#else
  pthread_t tid;
#endif
} http_thread_t;

static inline int create_listener_socket(
  const char *bind_ip,
  uint16_t port,
  socket_t *sock_out) {
  int opt = 1;
  struct sockaddr_in addr;

  if (sock_out == NULL) {
    return 1;
  }

  socket_t sock;
  socket_init(&sock);

#ifdef _WIN32
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == INVALID_SOCKET) {
#else
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
#endif
    sock_perror("socket");
    return 1;
  }

#ifdef _WIN32
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt,
                 sizeof(opt)) == SOCKET_ERROR) {
#else
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif
    sock_perror("setsockopt(SO_REUSEADDR)");
    socket_close(sock);
    return 1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (bind_ip == NULL || bind_ip[0] == '\0') {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
    fprintf(stderr, "invalid bind address: %s\n", bind_ip);
    socket_close(sock);
    return 1;
  }

#ifdef _WIN32
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
#else
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#endif
    sock_perror("bind");
    socket_close(sock);
    return 1;
  }

#ifdef _WIN32
  if (listen(sock, 4) == SOCKET_ERROR) {
#else
  if (listen(sock, 4) == -1) {
#endif
    sock_perror("listen");
    socket_close(sock);
    return 1;
  }

  *sock_out = sock;
  return 0;
}

#ifdef _WIN32
static unsigned __stdcall http_thread_main(void *arg) {
#else
static void *http_thread_main(void *arg) {
#endif
  http_thread_ctx_t *ctx = (http_thread_ctx_t *)arg;
  socket_t sock = ctx->sock;
  server_opt_t opt = ctx->opt;
  free(ctx);

  if (http_server(sock, &opt) != 0) {
    if (!shutdown_requested()) {
      fprintf(stderr, "http server stopped unexpectedly\n");
    }
  }

#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

static int start_http_thread(socket_t sock,
                             const server_opt_t *ser_opt,
                             http_thread_t *thread_out) {
  if (thread_out == NULL) {
    return 1;
  }

  http_thread_ctx_t *ctx = (http_thread_ctx_t *)malloc(sizeof(*ctx));
  if (ctx == NULL) {
    perror("malloc(http_thread_ctx)");
    return 1;
  }

  ctx->sock = sock;
  ctx->opt = *ser_opt;

#ifdef _WIN32
  uintptr_t handle = _beginthreadex(NULL, 0, http_thread_main, ctx, 0, NULL);
  if (handle == 0) {
    fprintf(stderr, "_beginthreadex(http) failed\n");
    free(ctx);
    return 1;
  }
  thread_out->handle = (HANDLE)handle;
#else
  int err = pthread_create(&thread_out->tid, NULL, http_thread_main, ctx);
  if (err != 0) {
    fprintf(stderr, "pthread_create(http): %s\n", strerror(err));
    free(ctx);
    return 1;
  }
#endif

  thread_out->started = 1;

  return 0;
}

static void join_http_thread(http_thread_t *thread) {
  if (thread == NULL || !thread->started) {
    return;
  }

#ifdef _WIN32
  (void)WaitForSingleObject(thread->handle, INFINITE);
  CloseHandle(thread->handle);
  thread->handle = NULL;
#else
  (void)pthread_join(thread->tid, NULL);
#endif

  thread->started = 0;
}

static int server_handle_text_message(socket_t conn,
                                      const protocol_header_t *proto_header,
                                      uint8_t *ack) {
  char *message = NULL;

  if (ack == NULL) {
    fprintf(stderr, "invalid text message handler arguments\n");
    return 1;
  }

  if (proto_header->flags != HF_MSG_FLAG_NONE) {
    fprintf(stderr, "protocol error: text message has unsupported flags\n");
    return 1;
  }

  if (proto_header->payload_size > HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE) {
    fprintf(stderr, "protocol error: text message too large\n");
    return 1;
  }

  size_t message_len = (size_t)proto_header->payload_size;
  message = (char *)malloc(message_len + 1u);
  if (message == NULL) {
    perror("malloc(message)");
    return 1;
  }

  if (message_len > 0) {
    ssize_t n = recv_all(conn, message, message_len);
    if (n != (ssize_t)message_len) {
      free(message);
      if (n < 0) {
        sock_perror("recv_all(message)");
      } else {
        fprintf(stderr,
                "protocol error: unexpected EOF while receiving message\n");
      }
      return 1;
    }
  }

  message[message_len] = '\0';
  if (message_store_set(message) != 0) {
    fprintf(stderr, "failed to store latest message\n");
    free(message);
    return 1;
  }
  free(message);
  *ack = 0;
  return 0;
}

typedef enum {
  SERVER_REPLY_ACK_SUCCESS = 0,
  SERVER_REPLY_ACK_FAILURE = 1,
  SERVER_REPLY_CLOSE = 2
} server_reply_t;

static int server_handle_file_transfer(socket_t conn,
                                       const server_opt_t *ser_opt,
                                       const protocol_header_t *proto_header,
                                       server_reply_t *reply) {
  int exit_code = 1;
  char *file_name = NULL;
  char full_path[4096];
  uint64_t content_size = 0;
  uint64_t prefix_size = 0;

  if (reply == NULL || ser_opt == NULL) {
    fprintf(stderr, "invalid file transfer handler arguments\n");
    return 1;
  }

  *reply = SERVER_REPLY_ACK_FAILURE;

  if (proto_header->payload_size <
      (uint64_t)proto_file_transfer_prefix_size(0)) {
    fprintf(stderr, "protocol error: payload size too small\n");
    return 1;
  }

  protocol_result_t header_res = proto_recv_file_transfer_prefix(conn,
    &file_name, &content_size);
  if (header_res != PROTOCOL_OK) {
    if (header_res == PROTOCOL_ERR_FILE_NAME_LEN) {
      fprintf(stderr, "protocol error: invalid file name length\n");
    } else if (header_res == PROTOCOL_ERR_ALLOC) {
      perror("malloc(file_name)");
    } else if (header_res == PROTOCOL_ERR_EOF) {
      fprintf(stderr,
              "protocol error: unexpected EOF while receiving payload\n");
    } else {
      sock_perror("protocol_recv_file_transfer_prefix");
    }
    goto CLEANUP;
  }

  if (fs_validate_file_name(file_name) != 0) {
    fprintf(stderr, "invalid file name: %s\n", file_name);
    goto CLEANUP;
  }

  prefix_size = (uint64_t)proto_file_transfer_prefix_size((uint16_t)strlen(file_name));
  if (proto_header->payload_size != prefix_size + content_size) {
    fprintf(stderr, "protocol error: payload size mismatch\n");
    goto CLEANUP;
  }

  uint8_t ready_ack = 0;
  ssize_t ready_ack_sent = send_all(conn, &ready_ack, sizeof(ready_ack));
  if (ready_ack_sent != (ssize_t)sizeof(ready_ack)) {
    sock_perror("send_all(file_transfer_ready_ack)");
    *reply = SERVER_REPLY_CLOSE;
    goto CLEANUP;
  }
  *reply = SERVER_REPLY_ACK_FAILURE;

  if (transfer_recv_socket_file(conn, ser_opt->path, file_name, content_size,
                                "recv(file_body)",
                                "protocol error: unexpected EOF while receiving file",
                                full_path, sizeof(full_path)) != 0) {
    goto CLEANUP;
  }

  *reply = SERVER_REPLY_ACK_SUCCESS;
  printf("saved to %s\n", full_path);
  fflush(stdout);
  exit_code = 0;

CLEANUP:
  if (file_name != NULL) free(file_name);
  return exit_code;
}

static int server_run_tcp(socket_t sock, const server_opt_t *ser_opt) {
  int exit_code = 0;
  ssize_t ack_sent = 0;
  (void)ser_opt;

  for (;;) {
    int ready = 0;
    if (shutdown_requested()) {
      exit_code = shutdown_exit_code();
      break;
    }

    if (net_wait_readable(sock, 250u, &ready) != 0) {
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
      sock_perror("select(accept)");
      exit_code = 1;
      continue;
    }
    if (!ready) {
      continue;
    }

#ifdef _WIN32
    SOCKET conn = accept(sock, NULL, NULL);
    if (conn == INVALID_SOCKET) {
      if (WSAGetLastError() == WSAEINTR) continue;
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
#else
    int conn = accept(sock, NULL, NULL);
    if (conn < 0) {
      if (errno == EINTR) continue;
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
#endif
      sock_perror("accept");
      continue;
    }

    server_reply_t reply = SERVER_REPLY_ACK_FAILURE;
    uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
    protocol_header_t proto_header = {0};

    protocol_result_t header_res = recv_header(conn, header_buf);
    if (header_res != PROTOCOL_OK) {
      if (header_res == PROTOCOL_ERR_EOF) {
        fprintf(stderr, "protocol error: unexpected EOF while receiving header\n");
      } else {
        sock_perror("recv_header");
      }
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    header_res = decode_header(&proto_header, header_buf);
    if (header_res != PROTOCOL_OK) {
      fprintf(stderr, "protocol error: failed to decode header\n");
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    switch (proto_header.msg_type) {
      case HF_MSG_TYPE_TEXT_MESSAGE: {
        uint8_t ack = 1;
        exit_code = server_handle_text_message(conn, &proto_header, &ack);
        reply = (ack == 0) ? SERVER_REPLY_ACK_SUCCESS : SERVER_REPLY_ACK_FAILURE;
        goto CLEANUP_CONN;
      }

      case HF_MSG_TYPE_FILE_TRANSFER: {
        exit_code = server_handle_file_transfer(conn, ser_opt, &proto_header,
                                                &reply);
        break;
      }

      default:
        fprintf(stderr, "protocol error: unsupported message type: %u\n",
                (unsigned)proto_header.msg_type);
        exit_code = 1;
        goto CLEANUP_CONN;
    }

    CLEANUP_CONN:
    if (reply != SERVER_REPLY_CLOSE) {
      uint8_t ack = (reply == SERVER_REPLY_ACK_SUCCESS) ? 0 : 1;
      ack_sent = send_all(conn, &ack, sizeof(ack));
      if (ack_sent != (ssize_t)sizeof(ack)) {
        sock_perror("send_all(ack)");
      }
    }
    socket_close(conn);
  }

  return exit_code;
}

static void server_print_startup_summary(const server_opt_t *ser_opt) {
  printf("HFile server ready\n");
  printf("  receive dir: %s\n", ser_opt->path);
  printf("  tcp listen : 0.0.0.0:%u\n", (unsigned)ser_opt->port);
  if (ser_opt->http_port != 0) {
    printf("  web ui     : http://%s:%u/\n",
           ser_opt->http_bind != NULL ? ser_opt->http_bind : "0.0.0.0",
           (unsigned)ser_opt->http_port);
  }
  printf("  status     : waiting for files and messages\n");
  fflush(stdout);
}

static void server_print_shutdown_notice(void) {
  int add_leading_newline = 0;

  if (shutdown_signal_number() == SIGINT) {
#ifdef _WIN32
    add_leading_newline = _isatty(_fileno(stderr)) ? 1 : 0;
#else
    add_leading_newline = isatty(fileno(stderr)) ? 1 : 0;
#endif
  }

  if (add_leading_newline) {
    fprintf(stderr, "\nshutdown requested, stopping server\n");
  } else {
    fprintf(stderr, "shutdown requested, stopping server\n");
  }
}

int server(const server_opt_t *ser_opt) {
  int exit_code = 0;
  int http_started = 0;
  http_thread_t http_thread = {0};

  if (ser_opt->path == NULL || strlen(ser_opt->path) == 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (message_store_init() != 0) {
    fprintf(stderr, "failed to initialize message store\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  socket_t sock;
  socket_t http_sock;
  socket_init(&sock);
  socket_init(&http_sock);

  if (create_listener_socket(NULL, ser_opt->port, &sock) != 0) {
    exit_code = 1;
    goto CLOSE_SOCK;
  }

  if (ser_opt->http_port != 0) {
    if (create_listener_socket(ser_opt->http_bind, ser_opt->http_port,
                               &http_sock) != 0) {
      exit_code = 1;
      goto CLOSE_SOCK;
    }
    if (start_http_thread(http_sock, ser_opt, &http_thread) != 0) {
      exit_code = 1;
      goto CLOSE_SOCK;
    }
    http_started = 1;
  }

  server_print_startup_summary(ser_opt);

  exit_code = server_run_tcp(sock, ser_opt);
  if (shutdown_requested()) {
    server_print_shutdown_notice();
  }

CLOSE_SOCK:
  shutdown_request();

  socket_close(http_sock);
  socket_close(sock);

  if (http_started) {
    join_http_thread(&http_thread);
  }

CLEAN_UP:
  message_store_cleanup();
  return exit_code;
}
