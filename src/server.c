#include "discovery.h"
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

#include <pthread.h>
#include <unistd.h>

#define SERVER_CONTROL_RECV_TIMEOUT_MS 15000u

typedef struct {
  socket_t conn;
  server_opt_t opt;
  server_conn_entry_t *entry;
} server_conn_ctx_t;

static protocol_result_t server_handle_file_transfer(
  socket_t conn,
  const server_opt_t *ser_opt,
  const protocol_header_t *proto_header);
static protocol_result_t server_send_response(socket_t conn,
                                              uint8_t phase,
                                              uint8_t status,
                                              protocol_result_t error_code);

static inline int create_listener_socket(
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

  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    sock_perror("setsockopt(SO_REUSEADDR)");
    socket_close(sock);
    return 1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    sock_perror("bind");
    socket_close(sock);
    return 1;
  }

  if (listen(sock, 4) == -1) {
    sock_perror("listen");
    socket_close(sock);
    return 1;
  }

  *sock_out = sock;
  return 0;
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

static void *server_connection_thread_main(void *arg) {
  server_conn_ctx_t *ctx = (server_conn_ctx_t *)arg;
  socket_t conn = ctx->conn;
  server_opt_t opt = ctx->opt;
  server_conn_entry_t *entry = ctx->entry;

  free(ctx);

  (void)handle_protocol_connection(conn, &opt);

  socket_close(conn);
  server_conn_tracker_end(entry);

  return NULL;
}

static int server_start_connection_thread(socket_t conn,
                                          const server_opt_t *ser_opt) {
  server_conn_ctx_t *ctx = (server_conn_ctx_t *)malloc(sizeof(*ctx));
  server_conn_entry_t *entry = NULL;
  if (ctx == NULL) {
    perror("malloc(server_conn_ctx)");
    return 1;
  }

  if (net_set_recv_timeout(conn, SERVER_CONTROL_RECV_TIMEOUT_MS) != 0) {
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

  pthread_t tid;
  int err = pthread_create(&tid, NULL, server_connection_thread_main, ctx);
  if (err != 0) {
    fprintf(stderr, "pthread_create(server_conn): %s\n", strerror(err));
    server_conn_tracker_end(entry);
    free(ctx);
    return 1;
  }
  (void)pthread_detach(tid);

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

  if (net_set_recv_timeout(
        conn, net_transfer_timeout_ms(content_size)) != 0) {
    sock_perror("setsockopt(SO_RCVTIMEO)");
    result = PROTOCOL_ERR_IO;
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

  fprintf(stdout, "received  %s  %llu bytes\n",
          file_name, (unsigned long long)content_size);
  fflush(stdout);

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

static int server_run_listener(socket_t tcp_sock, socket_t discovery_sock,
                                const server_opt_t *ser_opt) {
  int exit_code = 0;
  int has_discovery = is_socket_invalid(discovery_sock) ? 0 : 1;

  for (;;) {
    if (shutdown_requested()) {
      exit_code = shutdown_exit_code();
      break;
    }

    fd_set readfds;
    FD_ZERO(&readfds);

    FD_SET(tcp_sock, &readfds);
    if (has_discovery) {
      FD_SET(discovery_sock, &readfds);
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 50000;

    int maxfd = (int)tcp_sock;
    if (has_discovery) {
      if (discovery_sock > maxfd) maxfd = discovery_sock;
    }

    int rc = select(maxfd + 1, &readfds, NULL, NULL, &tv);
    if (rc < 0) {
      if (errno == EINTR) continue;
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
      sock_perror("select");
      exit_code = 1;
      continue;
    }

    if (rc == 0) continue;

    if (has_discovery) {
      int disc_ready = FD_ISSET(discovery_sock, &readfds) ? 1 : 0;
      if (disc_ready) {
        discovery_server_handle(discovery_sock, ser_opt->port);
      }
    }

    int tcp_ready = FD_ISSET(tcp_sock, &readfds) ? 1 : 0;
    if (!tcp_ready) continue;

    socket_t conn = accept(tcp_sock, NULL, NULL);
    if (is_socket_invalid(conn)) {
      if (errno == EINTR) continue;
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
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
  return (long)getpid();
}

static void server_print_access_details(const server_opt_t *ser_opt, long pid,
                                         int discovery_ok) {
  if (ser_opt == NULL || ser_opt->path == NULL) {
    return;
  }

  fprintf(stdout, "HFile server ready\n");
  fprintf(stdout, "  Receive Dir  %s\n", ser_opt->path);
  fprintf(stdout, "  Port         %u\n", (unsigned)ser_opt->port);
  fprintf(stdout, "  PID          %ld\n", pid);
  if (discovery_ok) {
    fprintf(stdout, "  Discovery    on (port %u)\n",
            (unsigned)(ser_opt->port + 1));
  }
  fflush(stdout);
}

static void server_print_shutdown_notice(void) {
  int add_leading_newline = 0;

  if (shutdown_signal_number() == SIGINT) {
    add_leading_newline = isatty(fileno(stderr)) ? 1 : 0;
  }

  if (add_leading_newline) {
    fprintf(stderr, "\nshutdown requested, stopping server\n");
  } else {
    fprintf(stderr, "shutdown requested, stopping server\n");
  }
}

static int server_run_process(const server_opt_t *ser_opt) {
  int exit_code = 0;
  int discovery_ok = 0;

  if (ser_opt->path == NULL || strlen(ser_opt->path) == 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (server_conn_tracker_init() != 0) {
    fprintf(stderr, "failed to initialize connection tracker\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  socket_t tcp_sock;
  socket_init(&tcp_sock);

  socket_t discovery_sock;
  socket_init(&discovery_sock);

  if (create_listener_socket(ser_opt->port, &tcp_sock) != 0) {
    exit_code = 1;
    goto CLOSE_SOCK;
  }

  if (ser_opt->port < 65535u) {
    if (discovery_server_open(&discovery_sock, (uint16_t)(ser_opt->port + 1u)) == 0) {
      discovery_ok = 1;
    }
  }

  server_print_access_details(ser_opt, server_current_pid_long(), discovery_ok);

  exit_code = server_run_listener(tcp_sock, discovery_sock, ser_opt);
  if (shutdown_requested()) {
    server_print_shutdown_notice();
  }

CLOSE_SOCK:
  shutdown_request();

  socket_close(tcp_sock);
  if (discovery_ok) {
    discovery_server_close(discovery_sock);
  }
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
