#include "cli.h"
#include "helper.h"
#include "net.h"
#include "protocol.h"
#include "client.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <fcntl.h>
#include "fs.h"
#include <unistd.h>
#include <sys/stat.h>


static int client_recv_ack(socket_t sock, uint64_t *perf_net_ns) {
  uint8_t ack = 1;
  uint64_t t_ack_start = now_ns();
  if (recv_all(sock, &ack, sizeof(ack)) != (ssize_t)sizeof(ack)) {
    *perf_net_ns += now_ns() - t_ack_start;
    sock_perror("recv_all(ack)");
    return 1;
  }
  *perf_net_ns += now_ns() - t_ack_start;
  if (ack != 0) {
    fprintf(stderr, "server reported transfer failure (ack=%u)\n", (unsigned)ack);
    return 1;
  }

  return 0;
}

static int client_connect(const char *ip, uint16_t port, socket_t *sock_out,
                          uint64_t *perf_net_ns) {
#ifdef _WIN32
  socket_t sock = INVALID_SOCKET;
#else
  socket_t sock = -1;
#endif

  struct sockaddr_in addr;

  sock = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
  if (sock == INVALID_SOCKET) {
#else
  if (sock == -1) {
#endif
    sock_perror("socket");
    return 1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    perror("inet_pton");
    socket_close(sock);
    return 1;
  }

  uint64_t t_connect_start = now_ns();
#ifdef _WIN32
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
#else
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#endif
    *perf_net_ns += now_ns() - t_connect_start;
    sock_perror("connect");
    socket_close(sock);
    return 1;
  }
  *perf_net_ns += now_ns() - t_connect_start;

  *sock_out = sock;
  return 0;
}

static int client_send_file_transfer(const client_opt_t *opt) {
  int exit_code = 0;

  //--perf
  uint64_t perf_start_ns = now_ns();
  uint64_t perf_io_ns = 0;
  uint64_t perf_net_ns = 0;
  uint64_t perf_file_bytes = 0;
  uint64_t perf_wire_bytes = 0;

  int in = -1;
  char *buf = NULL;
#ifdef _WIN32
  socket_t sock = INVALID_SOCKET;
#else
  socket_t sock = -1;
#endif
  const char *path = opt->path;
  const char *file_name = NULL;
  uint16_t file_name_len = 0;
  uint64_t content_size = 0;

  //TODO msg_flags
  if (opt->msg_flags != HF_MSG_FLAG_NONE) {
    fprintf(stderr, "unsupported flags for file transfer: %u\n",
            (unsigned)opt->msg_flags);
    return 1;
  }

  if (fs_basename_from_path(&path, &file_name) != 0) {
    fprintf(stderr, "invalid client path\n");
    exit_code = 1;
    goto CLEANUP;
  }

  if (proto_get_file_name_len(file_name, &file_name_len) != 0) {
    fprintf(stderr, "invalid file name length\n");
    exit_code = 1;
    goto CLEANUP;
  }

  uint64_t t_open_start = now_ns();
#ifdef _WIN32
  in = fs_open(path, O_RDONLY | O_BINARY, 0);
#else
  in = fs_open(path, O_RDONLY, 0);
#endif
  perf_io_ns += now_ns() - t_open_start;
  if (in == -1) {
    perror("open");
    exit_code = 1;
    goto CLEANUP;
  }

  if (proto_file_transfer_prefix_size(file_name_len) > CHUNK_SIZE) {
    fprintf(stderr, "protocol payload prefix exceeds buffer size\n");
    exit_code = 1;
    goto CLEANUP;
  }

#ifdef _WIN32
  struct _stat64 st;
  uint64_t t_stat_start = now_ns();
  if (_fstat64(in, &st) != 0) {
    perror("_fstat64");
#else
  struct stat st;
  uint64_t t_stat_start = now_ns();
  if (fstat(in, &st) != 0) {
    perror("fstat");
#endif
    perf_io_ns += now_ns() - t_stat_start;
    exit_code = 1;
    goto CLEANUP;
  }

  perf_io_ns += now_ns() - t_stat_start;
  if (st.st_size < 0) {
    fprintf(stderr, "invalid source file size\n");
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_connect(opt->ip, opt->port, &sock, &perf_net_ns) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  content_size = (uint64_t)st.st_size;
  perf_file_bytes = content_size;
  uint64_t payload_size =
    (uint64_t)proto_file_transfer_prefix_size(file_name_len) + content_size;
  perf_wire_bytes = (uint64_t)HF_PROTOCOL_HEADER_SIZE + payload_size;

  protocol_header_t header = {0};
  init_header(&header);
  header.msg_type = HF_MSG_TYPE_FILE_TRANSFER;
  header.flags = opt->msg_flags;
  header.payload_size = payload_size;

  uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
  protocol_result_t proto_res = encode_header(&header, header_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode protocol header\n");
    exit_code = 1;
    goto CLEANUP;
  }

  uint64_t t_header_start = now_ns();
  proto_res = send_header(sock, header_buf);
  perf_net_ns += now_ns() - t_header_start;
  if (proto_res != PROTOCOL_OK) {
    sock_perror("send_header");
    exit_code = 1;
    goto CLEANUP;
  }

  uint64_t t_prefix_start = now_ns();
  proto_res = proto_send_file_transfer_prefix(sock, file_name, content_size);
  perf_net_ns += now_ns() - t_prefix_start;
  if (proto_res != PROTOCOL_OK) {
    if (proto_res == PROTOCOL_ERR_FILE_NAME_LEN) {
      fprintf(stderr, "invalid file name length\n");
    } else {
      sock_perror("protocol_send_file_transfer_prefix");
    }
    exit_code = 1;
    goto CLEANUP;
  }

  size_t pos = 0;
  uint64_t remaining = content_size;
  buf = (char *)malloc(CHUNK_SIZE);
  if (buf == NULL) {
    perror("malloc(buf)");
    exit_code = 1;
    goto CLEANUP;
  }

  for (;;) {
    while (pos < CHUNK_SIZE && remaining > 0) {
      size_t want = CHUNK_SIZE - pos;
      if ((uint64_t)want > remaining) {
        want = (size_t)remaining;
      }

      uint64_t t_read_start = now_ns();
      ssize_t nr = fs_read(in, buf + pos, want);
      perf_io_ns += now_ns() - t_read_start;
      if (nr < 0) {
        perror("read");
        exit_code = 1;
        goto CLEANUP;
      }
      if (nr == 0) {
        fprintf(stderr, "source file changed during transfer\n");
        exit_code = 1;
        goto CLEANUP;
      }
      pos += (size_t)nr;
      remaining -= (uint64_t)nr;
    }

    if (pos > 0) {
      uint64_t t_send_start = now_ns();
      ssize_t sent = send_all(sock, buf, pos);
      perf_net_ns += now_ns() - t_send_start;
      if (sent != (ssize_t)pos) {
        sock_perror("send");
        exit_code = 1;
        goto CLEANUP;
      }
      pos = 0;
    }

    if (remaining == 0) {
      break;
    }
  }

#ifdef _WIN32
  if (shutdown(sock, SD_SEND) == SOCKET_ERROR) {
    sock_perror("shutdown(SD_SEND)");
  }
#else
  if (shutdown(sock, SHUT_WR) < 0) {
    sock_perror("shutdown(SHUT_WR)");
  }
#endif

  if (client_recv_ack(sock, &perf_net_ns) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

CLEANUP:
#ifdef _WIN32
  if (sock != INVALID_SOCKET)
    socket_close(sock);
#else
  if (sock != -1)
    socket_close(sock);
#endif
  if (buf != NULL) free(buf);
  if (in != -1) fs_close(in);

  if (opt->perf) {
    uint64_t perf_total_ns = now_ns() - perf_start_ns;
    report_transfer_perf(
      "client",
      exit_code == 0 ? 1 : 0,
      ns_to_s(perf_total_ns),
      ns_to_s(perf_io_ns),
      ns_to_s(perf_net_ns),
      perf_file_bytes,
      perf_wire_bytes);
  }

  return exit_code;
}

static int client_send_text_message(const client_opt_t *opt) {
  int exit_code = 0;
  uint64_t perf_start_ns = now_ns();
  uint64_t perf_io_ns = 0;
  uint64_t perf_net_ns = 0;
  uint64_t perf_file_bytes = 0;
  uint64_t perf_wire_bytes = 0;
#ifdef _WIN32
  socket_t sock = INVALID_SOCKET;
#else
  socket_t sock = -1;
#endif

  const char *message = opt->message;
  size_t message_len = 0;

  // TODO msg_flags
  if (opt->msg_flags != HF_MSG_FLAG_NONE) {
    fprintf(stderr, "unsupported flags for text message: %u\n",
            (unsigned)opt->msg_flags);
    return 1;
  }

  if (message == NULL) {
    fprintf(stderr, "invalid message\n");
    return 1;
  }

  message_len = strlen(message);
  if (message_len > HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE) {
    fprintf(stderr, "message too large\n");
    return 1;
  }

  if (client_connect(opt->ip, opt->port, &sock, &perf_net_ns) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  uint64_t payload_size = (uint64_t)message_len;
  perf_file_bytes = payload_size;
  perf_wire_bytes = (uint64_t)HF_PROTOCOL_HEADER_SIZE + payload_size;

  protocol_header_t header = {0};
  init_header(&header);
  header.msg_type = HF_MSG_TYPE_TEXT_MESSAGE;
  header.flags = opt->msg_flags;
  header.payload_size = payload_size;

  uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
  protocol_result_t proto_res = encode_header(&header, header_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode protocol header\n");
    exit_code = 1;
    goto CLEANUP;
  }

  uint64_t t_header_start = now_ns();
  proto_res = send_header(sock, header_buf);
  perf_net_ns += now_ns() - t_header_start;
  if (proto_res != PROTOCOL_OK) {
    sock_perror("send_header");
    exit_code = 1;
    goto CLEANUP;
  }

  if (message_len > 0) {
    uint64_t t_send_start = now_ns();
    ssize_t sent = send_all(sock, message, message_len);
    perf_net_ns += now_ns() - t_send_start;
    if (sent != (ssize_t)message_len) {
      sock_perror("send(message)");
      exit_code = 1;
      goto CLEANUP;
    }
  }

#ifdef _WIN32
  if (shutdown(sock, SD_SEND) == SOCKET_ERROR) {
    sock_perror("shutdown(SD_SEND)");
  }
#else
  if (shutdown(sock, SHUT_WR) < 0) {
    sock_perror("shutdown(SHUT_WR)");
  }
#endif

  if (client_recv_ack(sock, &perf_net_ns) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

CLEANUP:
#ifdef _WIN32
  if (sock != INVALID_SOCKET)
    socket_close(sock);
#else
  if (sock != -1)
    socket_close(sock);
#endif

  if (opt->perf) {
    uint64_t perf_total_ns = now_ns() - perf_start_ns;
    report_transfer_perf(
      "client",
      exit_code == 0 ? 1 : 0,
      ns_to_s(perf_total_ns),
      ns_to_s(perf_io_ns),
      ns_to_s(perf_net_ns),
      perf_file_bytes,
      perf_wire_bytes);
  }

  return exit_code;
}

int client(const client_opt_t *cli_opt) {
  if (cli_opt == NULL) {
    fprintf(stderr, "invalid client options\n");
    return 1;
  }

  switch (cli_opt->msg_type) {
    case HF_MSG_TYPE_FILE_TRANSFER:
      return client_send_file_transfer(cli_opt);
    case HF_MSG_TYPE_TEXT_MESSAGE:
      return client_send_text_message(cli_opt);
    default:
      fprintf(stderr, "unsupported client message type: %u\n",
              (unsigned)cli_opt->msg_type);
      return 1;
  }
}
