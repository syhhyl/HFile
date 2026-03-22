#include "cli.h"
#include "http.h"
#include "message_store.h"
#include "net.h"
#include "protocol.h"
#include "server.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "fs.h"
#include <fcntl.h>
#include "lz4.h"
#include <errno.h>

#ifdef _WIN32
  #include <process.h>
#else
  #include <pthread.h>
  #include <unistd.h>
#endif

typedef struct {
  socket_t sock;
  server_opt_t opt;
} http_thread_ctx_t;

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
    fprintf(stderr, "http server stopped unexpectedly\n");
  }

#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

static int start_http_thread(socket_t sock, const server_opt_t *ser_opt) {
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
  CloseHandle((HANDLE)handle);
#else
  pthread_t tid;
  int err = pthread_create(&tid, NULL, http_thread_main, ctx);
  if (err != 0) {
    fprintf(stderr, "pthread_create(http): %s\n", strerror(err));
    free(ctx);
    return 1;
  }
  (void)pthread_detach(tid);
#endif

  return 0;
}

static int server_discard_body_bytes(socket_t conn, uint64_t remaining) {
  char buf[4096];

  while (remaining > 0) {
    size_t want = sizeof(buf);
    if ((uint64_t)want > remaining) {
      want = (size_t)remaining;
    }

    ssize_t n = recv_all(conn, buf, want);
    if (n != (ssize_t)want) {
      return 1;
    }
    remaining -= (uint64_t)want;
  }

  return 0;
}

static int server_recv_compressed_file_body(socket_t conn, int out,
                                            uint64_t wire_body_size,
                                            uint64_t content_size) {
  int exit_code = 1;
  int drain_remaining = 0;
  char *raw_buf = NULL;
  char *stored_buf = NULL;
  uint64_t wire_remaining = wire_body_size;
  uint64_t raw_total = 0;
  int stored_cap = LZ4_compressBound((int)CHUNK_SIZE);

  raw_buf = (char *)malloc((size_t)CHUNK_SIZE);
  if (raw_buf == NULL) {
    perror("malloc(raw_buf)");
    goto CLEANUP;
  }

  if (stored_cap <= 0) {
    fprintf(stderr, "failed to size compressed receive buffer\n");
    goto CLEANUP;
  }

  stored_buf = (char *)malloc((size_t)stored_cap);
  if (stored_buf == NULL) {
    perror("malloc(stored_buf)");
    goto CLEANUP;
  }

  while (wire_remaining > 0) {
    uint8_t block_header[HF_COMPRESS_BLOCK_HEADER_SIZE];
    uint8_t block_type = 0;
    uint32_t raw_size = 0;
    uint32_t stored_size = 0;

    if (wire_remaining < HF_COMPRESS_BLOCK_HEADER_SIZE) {
      fprintf(stderr, "protocol error: compressed block header truncated\n");
      goto CLEANUP;
    }

    ssize_t n = recv_all(conn, block_header, sizeof(block_header));
    if (n != (ssize_t)sizeof(block_header)) {
      if (n < 0) {
        sock_perror("recv_all(compressed_block_header)");
      } else {
        fprintf(stderr,
                "protocol error: unexpected EOF while receiving compressed block header\n");
      }
      goto CLEANUP;
    }
    wire_remaining -= sizeof(block_header);

    if (proto_decode_compressed_block_header(block_header, &block_type,
                                             &raw_size, &stored_size) !=
        PROTOCOL_OK) {
      fprintf(stderr, "protocol error: failed to decode compressed block header\n");
      drain_remaining = 1;
      goto CLEANUP;
    }

    if (block_type != HF_COMPRESS_BLOCK_TYPE_RAW &&
        block_type != HF_COMPRESS_BLOCK_TYPE_LZ4) {
      fprintf(stderr, "protocol error: invalid compressed block type\n");
      drain_remaining = 1;
      goto CLEANUP;
    }
    if (raw_size == 0 || raw_size > (uint32_t)CHUNK_SIZE) {
      fprintf(stderr, "protocol error: invalid compressed block raw size\n");
      drain_remaining = 1;
      goto CLEANUP;
    }
    if (stored_size == 0 || stored_size > (uint32_t)stored_cap) {
      fprintf(stderr, "protocol error: invalid compressed block stored size\n");
      drain_remaining = 1;
      goto CLEANUP;
    }
    if ((uint64_t)stored_size > wire_remaining) {
      fprintf(stderr, "protocol error: compressed block size mismatch\n");
      drain_remaining = 1;
      goto CLEANUP;
    }
    if (raw_total > content_size || content_size - raw_total < (uint64_t)raw_size) {
      fprintf(stderr, "protocol error: decompressed size mismatch\n");
      drain_remaining = 1;
      goto CLEANUP;
    }

    n = recv_all(conn, stored_buf, stored_size);
    if (n != (ssize_t)stored_size) {
      if (n < 0) {
        sock_perror("recv_all(compressed_block_data)");
      } else {
        fprintf(stderr,
                "protocol error: unexpected EOF while receiving compressed block data\n");
      }
      goto CLEANUP;
    }
    wire_remaining -= stored_size;

    if (block_type == HF_COMPRESS_BLOCK_TYPE_RAW) {
      if (stored_size != raw_size) {
        fprintf(stderr, "protocol error: raw compressed block size mismatch\n");
        drain_remaining = 1;
        goto CLEANUP;
      }

      ssize_t nw = fs_write_all(out, stored_buf, stored_size);
      if (nw != (ssize_t)stored_size) {
        perror("write_all");
        drain_remaining = 1;
        goto CLEANUP;
      }
    } else {
      int decoded = LZ4_decompress_safe(stored_buf, raw_buf, (int)stored_size,
                                        (int)raw_size);
      if (decoded != (int)raw_size) {
        fprintf(stderr, "protocol error: failed to decompress file block\n");
        drain_remaining = 1;
        goto CLEANUP;
      }

      ssize_t nw = fs_write_all(out, raw_buf, raw_size);
      if (nw != (ssize_t)raw_size) {
        perror("write_all");
        drain_remaining = 1;
        goto CLEANUP;
      }
    }

    raw_total += (uint64_t)raw_size;
  }

  if (raw_total != content_size) {
    fprintf(stderr, "protocol error: decompressed size mismatch\n");
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  if (drain_remaining && wire_remaining > 0) {
    (void)server_discard_body_bytes(conn, wire_remaining);
  }
  if (stored_buf != NULL) free(stored_buf);
  if (raw_buf != NULL) free(raw_buf);
  return exit_code;
}

static int server_handle_text_message(socket_t conn,
                                      const protocol_header_t *proto_header,
                                      uint8_t *ack) {
  char *message = NULL;

  if (ack == NULL) {
    fprintf(stderr, "invalid text message handler arguments\n");
    return 1;
  }

  if (proto_header->flags == HF_MSG_FLAG_COMPRESS) {
    fprintf(stderr, "protocol error: text message has unsupported compress flag\n");
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
  char *buf = NULL;
  char tmp_path[4096];
  int out = -1;
  int body_complete = 0;
  uint64_t content_size = 0;
  uint64_t wire_body_size = 0;
  uint16_t file_len = 0;
  int compressed_transfer = 0;

  tmp_path[0] = '\0';

  if (reply == NULL || ser_opt == NULL) {
    fprintf(stderr, "invalid file transfer handler arguments\n");
    return 1;
  }

  *reply = SERVER_REPLY_ACK_FAILURE;
  compressed_transfer = (proto_header->flags & HF_MSG_FLAG_COMPRESS) != 0;

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

  file_len = (uint16_t)strlen(file_name);

  if (fs_validate_file_name(file_name) != 0) {
    fprintf(stderr, "invalid file name: %s\n", file_name);
    goto CLEANUP;
  }

  uint64_t prefix_size = (uint64_t)proto_file_transfer_prefix_size(file_len);
  if (compressed_transfer) {
    if (proto_header->payload_size < prefix_size) {
      fprintf(stderr, "protocol error: payload size mismatch\n");
      goto CLEANUP;
    }
    wire_body_size = proto_header->payload_size - prefix_size;
  } else {
    uint64_t expected_payload_size = prefix_size + content_size;
    if (proto_header->payload_size != expected_payload_size) {
      fprintf(stderr, "protocol error: payload size mismatch\n");
      goto CLEANUP;
    }
    wire_body_size = content_size;
  }

  char full_path[4096];
  int full_n = 0;
  if (fs_join_path(full_path, sizeof(full_path), ser_opt->path, file_name) != 0) {
    full_n = -1;
  } else {
    full_n = (int)strlen(full_path);
  }

  if (full_n < 0 || (size_t)full_n >= sizeof(full_path)) {
    fprintf(stderr, "output path is too long\n");
    goto CLEANUP;
  }
#ifdef _WIN32
  int pid = _getpid();
#else
  int pid = (int)getpid();
#endif
  for (int attempt = 0; attempt < 16; attempt++) {
    if (fs_make_temp_path(tmp_path, sizeof(tmp_path), full_path, pid,
                          attempt) != 0) {
      fprintf(stderr, "temporary file path is too long\n");
      goto CLEANUP;
    }

    out = fs_open_temp_file(tmp_path);
    if (out != -1) break;
    if (errno == EEXIST) continue;
    perror("open(temp)");
    goto CLEANUP;
  }

  if (out == -1) {
    fprintf(stderr, "failed to create temporary file\n");
    goto CLEANUP;
  }

  if (!compressed_transfer) {
    buf = (char *)malloc(CHUNK_SIZE);
    if (buf == NULL) {
      perror("malloc(buf)");
      goto CLEANUP;
    }
  }

  uint8_t ready_ack = 0;
  ssize_t ready_ack_sent = send_all(conn, &ready_ack, sizeof(ready_ack));
  if (ready_ack_sent != (ssize_t)sizeof(ready_ack)) {
    sock_perror("send_all(file_transfer_ready_ack)");
    *reply = SERVER_REPLY_CLOSE;
    goto CLEANUP;
  }
  *reply = SERVER_REPLY_ACK_FAILURE;

  if (compressed_transfer) {
    if (server_recv_compressed_file_body(conn, out, wire_body_size,
                                         content_size) != 0) {
      body_complete = 0;
    } else {
      body_complete = 1;
    }
  } else {
    uint64_t remaining = content_size;

    while (remaining > 0) {
      ssize_t n = 0;
      size_t want = CHUNK_SIZE;
      if (remaining < (uint64_t)want)
        want = (size_t)remaining;

#ifdef _WIN32
      int tmp = recv(conn, buf, (int)want, 0);
      if (tmp == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        sock_perror("recv");
        break;
      }
      n = (ssize_t)tmp;
#else
      n = recv(conn, buf, want, 0);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        sock_perror("recv");
        break;
      }
#endif
      if (n == 0) {
        fprintf(stderr, "protocol error: unexpected EOF while receiving file\n");
        break;
      }

      ssize_t nw = fs_write_all(out, buf, (size_t)n);
      if (nw != (ssize_t)n) {
        perror("write_all");
        remaining -= (uint64_t)n;
        if (server_discard_body_bytes(conn, remaining) != 0) {
          *reply = SERVER_REPLY_CLOSE;
        }
        break;
      }
      remaining -= (uint64_t)n;
    }

    if (remaining == 0) {
      body_complete = 1;
    }
  }

  free(buf);
  buf = NULL;
  fs_close(out);
  out = -1;

  if (body_complete) {
    *reply = SERVER_REPLY_ACK_FAILURE;
    unsigned long win_err = 0;
    if (fs_finalize_temp_file(tmp_path, full_path, &win_err) != 0) {
#ifdef _WIN32
      fprintf(stderr, "failed to finalize temporary file (err=%lu)\n",
              (unsigned long)win_err);
#else
      perror("rename");
#endif
      fs_remove_quiet(tmp_path);
    } else {
      *reply = SERVER_REPLY_ACK_SUCCESS;
      printf("saved to %s\n", full_path);
      fflush(stdout);
      exit_code = 0;
    }
  }

CLEANUP:
  if (reply != NULL && *reply != SERVER_REPLY_ACK_SUCCESS && tmp_path[0] != '\0') {
    fs_remove_quiet(tmp_path);
  }
  if (buf != NULL) free(buf);
  if (out != -1) fs_close(out);
  if (file_name != NULL) free(file_name);
  return exit_code;
}

static int server_run_tcp(socket_t sock, const server_opt_t *ser_opt) {
  int exit_code = 0;
  ssize_t ack_sent = 0;

  for (;;) {
#ifdef _WIN32
    SOCKET conn = accept(sock, NULL, NULL);
    if (conn == INVALID_SOCKET) {
      if (WSAGetLastError() == WSAEINTR) continue;
#else
    int conn = accept(sock, NULL, NULL);
    if (conn < 0) {
      if (errno == EINTR) continue;
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

int server(const server_opt_t *ser_opt) {
  int exit_code = 0;
  int http_started = 0;

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
    if (start_http_thread(http_sock, ser_opt) != 0) {
      exit_code = 1;
      goto CLOSE_SOCK;
    }
    http_started = 1;
  }

  server_print_startup_summary(ser_opt);

  exit_code = server_run_tcp(sock, ser_opt);

CLOSE_SOCK:
  if (!http_started) {
    socket_close(http_sock);
  }
  socket_close(sock);

CLEAN_UP:
  message_store_cleanup();
  return exit_code;
}
