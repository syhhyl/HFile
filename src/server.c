#include "cli.h"
#include "net.h"
#include "protocol.h"
#include "shutdown.h"
#include "server.h"
#include "server_conn_tracker.h"
#include "transfer_io.h"

#include <stddef.h>
#include <fcntl.h>
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
  socket_t conn;
  server_opt_t opt;
  server_conn_entry_t *entry;
} server_conn_ctx_t;

typedef enum {
  SERVER_CONN_KIND_INVALID = 0,
  SERVER_CONN_KIND_PROTOCOL,
} server_conn_kind_t;

typedef struct {
#ifdef _WIN32
  HANDLE handle;
#else
  pthread_t tid;
#endif
} server_thread_t;

static protocol_result_t server_handle_file_transfer(
  socket_t conn,
  const server_opt_t *ser_opt,
  const protocol_header_t *proto_header);
static protocol_result_t server_send_response(socket_t conn,
                                              uint8_t phase,
                                              uint8_t status,
                                              protocol_result_t error_code);

static int server_set_connection_recv_timeout(socket_t conn, uint32_t timeout_ms) {
#ifdef _WIN32
  DWORD timeout = (DWORD)timeout_ms;
  return setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO,
                    (const char *)&timeout, sizeof(timeout)) == SOCKET_ERROR ? 1 : 0;
#else
  struct timeval tv;
  tv.tv_sec = (time_t)(timeout_ms / 1000u);
  tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
  return setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ? 1 : 0;
#endif
}

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

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (is_socket_invalid(sock)) {
    sock_perror("socket");
    return 1;
  }

#ifdef _WIN32
  if (setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&opt,
                 sizeof(opt)) == SOCKET_ERROR) {
#else
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif
#ifdef _WIN32
    sock_perror("setsockopt(SO_EXCLUSIVEADDRUSE)");
#else
    sock_perror("setsockopt(SO_REUSEADDR)");
#endif
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

static server_conn_kind_t server_detect_connection_kind(socket_t conn) {
  uint8_t buf[8];
  ssize_t n = 0;

#ifdef _WIN32
  n = recv(conn, (char *)buf, (int)sizeof(buf), MSG_PEEK);
  if (n == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err == WSAEINTR || err == WSAEWOULDBLOCK) {
      return SERVER_CONN_KIND_INVALID;
    }
    return SERVER_CONN_KIND_INVALID;
  }
#else
  n = recv(conn, buf, sizeof(buf), MSG_PEEK);
  if (n < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      return SERVER_CONN_KIND_INVALID;
    }
    return SERVER_CONN_KIND_INVALID;
  }
#endif
  if (n < 2) {
    return SERVER_CONN_KIND_INVALID;
  }

  if (buf[0] == (uint8_t)(HF_PROTOCOL_MAGIC >> 8) &&
      buf[1] == (uint8_t)(HF_PROTOCOL_MAGIC & 0xFFu)) {
    return SERVER_CONN_KIND_PROTOCOL;
  }

  return SERVER_CONN_KIND_PROTOCOL;
}

static int handle_protocol_connection(socket_t conn,
                                             const server_opt_t *ser_opt) {
  uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
  protocol_header_t proto_header = {0};

  protocol_result_t prefix_res = recv_header(conn, header_buf);
  if (prefix_res != PROTOCOL_OK) {
    if (prefix_res == PROTOCOL_ERR_EOF) {
      fprintf(stderr, "protocol error: unexpected EOF while receiving header\n");
    } else {
      sock_perror("recv_header");
    }
    return 1;
  }

  prefix_res = decode_header(&proto_header, header_buf);
  if (prefix_res != PROTOCOL_OK) {
    fprintf(stderr, "protocol error: failed to decode header\n");
    return 1;
  }

  switch (proto_header.msg_type) {
    case HF_MSG_TYPE_SEND_FILE:
      return server_handle_file_transfer(conn, ser_opt, &proto_header) == PROTOCOL_OK ? 0 : 1;
  }

  return 1;
}

#ifdef _WIN32
static unsigned __stdcall server_connection_thread_main(void *arg) {
#else
static void *server_connection_thread_main(void *arg) {
#endif
  server_conn_ctx_t *ctx = (server_conn_ctx_t *)arg;
  socket_t conn = ctx->conn;
  server_opt_t opt = ctx->opt;
  server_conn_entry_t *entry = ctx->entry;

  free(ctx);

  switch (server_detect_connection_kind(conn)) {
    case SERVER_CONN_KIND_PROTOCOL:
      (void)handle_protocol_connection(conn, &opt);
      break;
    default:
      fprintf(stderr, "we don't support this mode\n");
      break;
  }

  socket_close(conn);
  server_conn_tracker_end(entry);

#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

static int server_start_connection_thread(socket_t conn,
                                          const server_opt_t *ser_opt) {
  server_conn_ctx_t *ctx = (server_conn_ctx_t *)malloc(sizeof(*ctx));
  server_conn_entry_t *entry = NULL;
  if (ctx == NULL) {
    perror("malloc(server_conn_ctx)");
    return 1;
  }

  if (server_set_connection_recv_timeout(conn, 15000u) != 0) {
    sock_perror("setsockopt(SO_RCVTIMEO)");
  }

  entry = server_conn_tracker_begin(conn);
  if (entry == NULL) {
    free(ctx);
    return 1;
  }

  ctx->conn = conn;
  ctx->opt = *ser_opt;
  ctx->entry = entry;

#ifdef _WIN32
  uintptr_t handle = _beginthreadex(NULL, 0, server_connection_thread_main, ctx, 0, NULL);
  if (handle == 0) {
    fprintf(stderr, "_beginthreadex(server_conn) failed\n");
    server_conn_tracker_end(entry);
    free(ctx);
    return 1;
  }
  CloseHandle((HANDLE)handle);
#else
  server_thread_t thread = {0};
  int err = pthread_create(&thread.tid, NULL, server_connection_thread_main, ctx);
  if (err != 0) {
    fprintf(stderr, "pthread_create(server_conn): %s\n", strerror(err));
    server_conn_tracker_end(entry);
    free(ctx);
    return 1;
  }
  (void)pthread_detach(thread.tid);
#endif

  return 0;
}

static protocol_result_t server_send_response(
  socket_t conn,
  uint8_t phase,
  uint8_t status,
  protocol_result_t error_code) {
  res_frame_t frame = {0};

  frame.phase = phase;
  frame.status = status;
  frame.error_code = (uint16_t)error_code;
  return send_res_frame(conn, &frame);
}

static protocol_result_t server_handle_file_transfer(
  socket_t conn,
  const server_opt_t *ser_opt,
  const protocol_header_t *proto_header) {
  char *file_name = NULL;
  char saved_path[4096];
  uint64_t content_size = 0;
  uint64_t prefix_size = 0;
  protocol_result_t result = PROTOCOL_ERR_IO;

  if (ser_opt == NULL) {
    fprintf(stderr, "invalid file transfer handler arguments\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (proto_header->payload_size <
      (uint64_t)proto_file_transfer_prefix_size(1)) {
    fprintf(stderr, "protocol error: payload size too small\n");
    (void)server_send_response(
      conn, PROTO_PHASE_READY, PROTO_STATUS_REJECTED, PROTOCOL_ERR_INVALID_ARGUMENT);
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  result = proto_recv_file_transfer_prefix(conn, &file_name, &content_size);
  if (result != PROTOCOL_OK) {
    if (result == PROTOCOL_ERR_FILE_NAME_LEN) {
      fprintf(stderr, "protocol error: invalid file name length\n");
    } else if (result == PROTOCOL_ERR_ALLOC) {
      perror("malloc(file_name)");
    } else if (result == PROTOCOL_ERR_EOF) {
      fprintf(stderr,
              "protocol error: unexpected EOF while receiving payload\n");
    } else {
      sock_perror("protocol_recv_file_transfer_prefix");
    }
    goto CLEANUP;
  }

  if (fs_validate_file_name(file_name) != 0) {
    fprintf(stderr, "invalid file name: %s\n", file_name);
    result = PROTOCOL_ERR_INVALID_FILE_NAME;
    goto SEND_READY_REJECT;
  }

  prefix_size = (uint64_t)proto_file_transfer_prefix_size((uint16_t)strlen(file_name));
  if (proto_header->payload_size != prefix_size + content_size) {
    fprintf(stderr, "protocol error: payload size mismatch\n");
    result = PROTOCOL_ERR_PAYLOAD_SIZE_MISMATCH;
    goto SEND_READY_REJECT;
  }

  result = server_send_response(
    conn, PROTO_PHASE_READY, PROTO_STATUS_OK, PROTOCOL_OK);
  if (result != PROTOCOL_OK) {
    sock_perror("send_res_frame(file_transfer_ready)");
    goto CLEANUP;
  }

  result = transfer_recv_socket_file(conn, ser_opt->path, file_name, content_size,
                                     "recv(file_body)",
                                     "protocol error: unexpected EOF while receiving file",
                                     saved_path, sizeof(saved_path));
  if (result != PROTOCOL_OK) {
    if (server_send_response(
          conn, PROTO_PHASE_FINAL, PROTO_STATUS_FAILED, result) != PROTOCOL_OK) {
      sock_perror("send_res_frame(file_transfer_final_failed)");
    }
    goto CLEANUP;
  }

  result = server_send_response(
    conn, PROTO_PHASE_FINAL, PROTO_STATUS_OK, PROTOCOL_OK);
  if (result != PROTOCOL_OK) {
    sock_perror("send_res_frame(file_transfer_final_ok)");
    goto CLEANUP;
  }

  result = PROTOCOL_OK;
  goto CLEANUP;

SEND_READY_REJECT:
  if (server_send_response(
        conn, PROTO_PHASE_READY, PROTO_STATUS_REJECTED, result) != PROTOCOL_OK) {
    sock_perror("send_res_frame(file_transfer_ready_rejected)");
  }

CLEANUP:
  if (file_name != NULL) free(file_name);
  return result;
}

static int server_run_listener(socket_t sock, const server_opt_t *ser_opt) {
  int exit_code = 0;

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

    socket_t conn = accept(sock, NULL, NULL);
#ifdef _WIN32
    if (is_socket_invalid(conn)) {
      if (WSAGetLastError() == WSAEINTR) continue;
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
#else
    if (is_socket_invalid(conn)) {
      if (errno == EINTR) continue;
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
#endif
      sock_perror("accept");
      continue;
    }

    if (server_start_connection_thread(conn, ser_opt) != 0) {
      socket_close(conn);
      exit_code = 1;
    }
  }

  return exit_code;
}

static long server_current_pid_long(void) {
#ifdef _WIN32
  return (long)_getpid();
#else
  return (long)getpid();
#endif
}

static void server_print_access_details(const server_opt_t *ser_opt, long pid) {
  if (ser_opt == NULL || ser_opt->path == NULL) {
    return;
  }

  fprintf(stdout, "HFile server ready\n");
  fprintf(stdout, "  Receive Dir  %s\n", ser_opt->path);
  fprintf(stdout, "  Port         %u\n", (unsigned)ser_opt->port);
  fprintf(stdout, "  PID          %ld\n", pid);
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

static int server_run_process(const server_opt_t *ser_opt) {
  int exit_code = 0;

  if (ser_opt->path == NULL || strlen(ser_opt->path) == 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (server_conn_tracker_init() != 0) {
    fprintf(stderr, "failed to initialize connection tracker\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  socket_t sock;
  socket_init(&sock);

  if (create_listener_socket(NULL, ser_opt->port, &sock) != 0) {
    exit_code = 1;
    goto CLOSE_SOCK;
  }

  server_print_access_details(ser_opt, server_current_pid_long());

  exit_code = server_run_listener(sock, ser_opt);
  if (shutdown_requested()) {
    server_print_shutdown_notice();
  }

CLOSE_SOCK:
  shutdown_request();

  socket_close(sock);
  server_conn_tracker_shutdown_all();
  server_conn_tracker_wait_idle();

CLEAN_UP:
  server_conn_tracker_cleanup();
  return exit_code;
}

int server(const server_opt_t *ser_opt) {
  if (ser_opt == NULL) {
    fprintf(stderr, "invalid server options\n");
    return 1;
  }
  return server_run_process(ser_opt);
}
