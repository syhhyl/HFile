#include "cli.h"
#include "net.h"
#include "protocol.h"
#include "server.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "fs.h"
#include <fcntl.h>
#include "helper.h"

#include <errno.h>
#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif


int server(const server_opt_t *ser_opt) {
  int exit_code = 0;
  int opt = 1;
#ifdef _WIN32
  SOCKET sock = INVALID_SOCKET;
#else
  int sock = -1;
#endif

  if (ser_opt->path == NULL || strlen(ser_opt->path) == 0) {
    exit_code = 1;
    goto CLOSE_SOCK;
  }

#ifdef _WIN32
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == INVALID_SOCKET) {
#else
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
#endif
    sock_perror("socket");
    exit_code = 1;
    goto CLOSE_SOCK;
  }

#ifdef _WIN32
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
#else
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif
    sock_perror("setsockopt(SO_REUSEADDR)");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ser_opt->port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

#ifdef _WIN32
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
#else
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#endif
    sock_perror("bind");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  
#ifdef _WIN32
  if (listen(sock, 4) == SOCKET_ERROR) {
#else
  if (listen(sock, 4) == -1) {
#endif
    sock_perror("listen");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  

  printf("listening on %s port %u\n", ser_opt->path, (unsigned)ser_opt->port);
  fflush(stdout);

  int conn_flag = 1;
  while (conn_flag) {
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

    uint8_t ack = 1;
    uint64_t perf_conn_start_ns = now_ns();
    uint64_t perf_io_ns = 0;
    uint64_t perf_net_ns = 0;
    uint64_t perf_file_bytes = 0;
    uint64_t perf_wire_bytes = 0;
    uint64_t t_ack_start = 0;
    char *file_name = NULL;
    char *buf = NULL;
    uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
    protocol_header_t proto_header = {0};
    uint64_t content_size = 0;
    uint16_t file_len = 0;
    char tmp_path[4096];
    tmp_path[0] = '\0';
    int out = -1;
    int ok = 0;

    uint64_t t_recv_header_start = now_ns();
    protocol_result_t header_res = recv_header(conn, header_buf);
    perf_net_ns += now_ns() - t_recv_header_start;
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

    if (proto_header.magic != HF_PROTOCOL_MAGIC) {
      fprintf(stderr, "protocol error: invalid protocol magic\n");
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    if (proto_header.version != HF_PROTOCOL_VERSION) {
      fprintf(stderr, "protocol error: unsupported protocol version\n");
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    if (proto_header.flags != HF_MSG_FLAG_NONE) {
      fprintf(stderr, "protocol error: unsupported flags: %u\n",
              (unsigned)proto_header.flags);
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    perf_wire_bytes = (uint64_t)HF_PROTOCOL_HEADER_SIZE + proto_header.payload_size;

    if (proto_header.msg_type == HF_MSG_TYPE_TEXT_MESSAGE) {
      if (proto_header.payload_size > ((uint64_t)SIZE_MAX - 1u)) {
        fprintf(stderr, "protocol error: message payload too large\n");
        exit_code = 1;
        goto CLEANUP_CONN;
      }
      size_t msg_len = (size_t)proto_header.payload_size;
      char *message = (char *)malloc(msg_len + 1u);
      if (message == NULL) {
        perror("malloc(message)");
        exit_code = 1;
        goto CLEANUP_CONN;
      }

      if (msg_len > 0) {
        uint64_t t_recv_msg_start = now_ns();
        ssize_t n = recv_all(conn, message, msg_len);
        perf_net_ns += now_ns() - t_recv_msg_start;
        if (n != (ssize_t)msg_len) {
          free(message);
          if (n < 0) {
            sock_perror("recv_all(message)");
          } else {
            fprintf(stderr, "protocol error: unexpected EOF while receiving message\n");
          }
          exit_code = 1;
          goto CLEANUP_CONN;
        }
      }

      message[msg_len] = '\0';
      perf_file_bytes = (uint64_t)msg_len;
      printf("message: %s\n", message);
      fflush(stdout);
      free(message);
      ack = 0;
      goto CLEANUP_CONN;
    }

    if (proto_header.msg_type != HF_MSG_TYPE_FILE_TRANSFER) {
      fprintf(stderr, "protocol error: unsupported message type: %u\n",
              (unsigned)proto_header.msg_type);
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    if (proto_header.payload_size <
        (uint64_t)protocol_file_transfer_prefix_size(0)) {
      fprintf(stderr, "protocol error: payload size too small\n");
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    uint64_t t_recv_payload_start = now_ns();
    header_res = protocol_recv_file_transfer_prefix(conn, &file_name,
                                                    &content_size);
    perf_net_ns += now_ns() - t_recv_payload_start;
    if (header_res != PROTOCOL_OK) {
      if (header_res == PROTOCOL_ERR_FILE_NAME_LEN) {
        fprintf(stderr, "protocol error: invalid file name length\n");
      } else if (header_res == PROTOCOL_ERR_ALLOC) {
        perror("malloc(file_name)");
      } else if (header_res == PROTOCOL_ERR_EOF) {
        fprintf(stderr, "protocol error: unexpected EOF while receiving payload\n");
      } else {
        sock_perror("protocol_recv_file_transfer_prefix");
      }
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    file_len = (uint16_t)strlen(file_name);

    if (fs_validate_file_name(file_name) != 0) {
      fprintf(stderr, "invalid file name: %s\n", file_name);
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    uint64_t expected_payload_size =
      (uint64_t)protocol_file_transfer_prefix_size(file_len) + content_size;
    if (proto_header.payload_size != expected_payload_size) {
      fprintf(stderr, "protocol error: payload size mismatch\n");
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    perf_file_bytes = content_size;

    char full_path[4096];
    int full_n = 0;
    if (fs_join_path(full_path, sizeof(full_path), ser_opt->path, file_name) != 0) {
      full_n = -1;
    } else {
      full_n = (int)strlen(full_path);
    }

    if (full_n < 0 || (size_t)full_n >= sizeof(full_path)) {
      fprintf(stderr, "output path is too long\n");
      exit_code = 1;
      goto CLEANUP_CONN;
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
        exit_code = 1;
        goto CLEANUP_CONN;
      }

      uint64_t t_open_start = now_ns();
      out = fs_open_temp_file(tmp_path);
      perf_io_ns += now_ns() - t_open_start;
      if (out != -1) break;
      if (errno == EEXIST) continue;
      perror("open(temp)");
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    if (out == -1) {
      fprintf(stderr, "failed to create temporary file\n");
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    buf = (char *)malloc(CHUNK_SIZE);
    if (buf == NULL) {
      perror("malloc(buf)");
      exit_code = 1;
      goto CLEANUP_CONN;
    }

    uint64_t remaining = content_size;

    //recv loop
    while (remaining > 0) {
      ssize_t n = 0;
      size_t want = CHUNK_SIZE;
      if (remaining < (uint64_t)want) want = (size_t)remaining;

#ifdef _WIN32
      uint64_t t_recv_chunk_start = now_ns();
      int tmp = recv(conn, buf, (int)want, 0);
      if (tmp == SOCKET_ERROR) {
        perf_net_ns += now_ns() - t_recv_chunk_start;
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        sock_perror("recv");
        exit_code = 1;
        ok = 0;
        break;
      }
      perf_net_ns += now_ns() - t_recv_chunk_start;
      n = (ssize_t)tmp;
#else
      uint64_t t_recv_chunk_start = now_ns();
      n = recv(conn, buf, want, 0);
      if (n < 0) {
        perf_net_ns += now_ns() - t_recv_chunk_start;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        sock_perror("recv");
        exit_code = 1;
        ok = 0;
        break;
      }
      perf_net_ns += now_ns() - t_recv_chunk_start;
#endif
      if (n == 0) {
        fprintf(stderr, "protocol error: unexpected EOF while receiving file\n");
        exit_code = 1;
        ok = 0;
        break;
      }

      uint64_t t_write_start = now_ns();
      ssize_t nw = fs_write_all(out, buf, (size_t)n);
      perf_io_ns += now_ns() - t_write_start;
      if (nw != (ssize_t)n) {
        perror("write_all");
        exit_code = 1;
        ok = 0;
        break;
      }
      remaining -= (uint64_t)n;
    }

    if (remaining == 0) {
      ok = 1;
    }

    free(buf);
    buf = NULL;
    fs_close(out);
    out = -1;

    if (ok) {
      unsigned long win_err = 0;
      uint64_t t_rename_start = now_ns();
      if (fs_finalize_temp_file(tmp_path, full_path, &win_err) != 0) {
        perf_io_ns += now_ns() - t_rename_start;
#ifdef _WIN32
        fprintf(stderr, "failed to finalize temporary file (err=%lu)\n",
                (unsigned long)win_err);
#else
        perror("rename");
#endif
        fs_remove_quiet(tmp_path);
        exit_code = 1;
      } else {
        perf_io_ns += now_ns() - t_rename_start;
        ack = 0;
        printf("saved to %s\n", full_path);
        fflush(stdout);
      }
    }

CLEANUP_CONN:
    if (ack != 0 && tmp_path[0] != '\0') {
      fs_remove_quiet(tmp_path);
    }

    if (buf != NULL) free(buf);
    if (out != -1) fs_close(out);
    if (file_name != NULL) free(file_name);

    t_ack_start = now_ns();
    ssize_t ack_sent = send_all(conn, &ack, sizeof(ack));
    perf_net_ns += now_ns() - t_ack_start;
    if (ack_sent != (ssize_t)sizeof(ack)) {
      sock_perror("send_all(ack)");
    }

    if (ser_opt->perf) {
      uint64_t perf_total_ns = now_ns() - perf_conn_start_ns;
      report_transfer_perf(
        "server",
        ack == 0 ? 1 : 0,
        ns_to_s(perf_total_ns),
        ns_to_s(perf_io_ns),
        ns_to_s(perf_net_ns),
        perf_file_bytes,
        perf_wire_bytes);
    }
    socket_close(conn);

  }

CLOSE_SOCK:
  if (sock !=
#ifdef _WIN32
      INVALID_SOCKET
#else
      -1
#endif
  ) {
    socket_close(sock);
  }

  return exit_code;
}
