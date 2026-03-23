#include "cli.h"
#include "net.h"
#include "protocol.h"
#include "client.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include "fs.h"
#include <sys/stat.h>
#include "lz4.h"


static int client_recv_ack(socket_t sock, const char *recv_ctx,
                           const char *failure_ctx) {
  uint8_t ack = 1;
  if (recv_all(sock, &ack, sizeof(ack)) != (ssize_t)sizeof(ack)) {
    sock_perror(recv_ctx);
    return 1;
  }
  if (ack != 0) {
    fprintf(stderr, "%s (ack=%u)\n", failure_ctx, (unsigned)ack);
    return 1;
  }

  return 0;
}

static int client_recv_file_final_result(socket_t sock) {
  uint8_t ack = 1;
  ssize_t n = recv_all(sock, &ack, sizeof(ack));
  if (n == (ssize_t)sizeof(ack)) {
    if (ack == 0) {
      return 0;
    }
    fprintf(stderr, "server reported transfer failure (ack=%u)\n",
            (unsigned)ack);
    return 1;
  }

  if (n == 0) {
    fprintf(stderr, "server aborted transfer after ready ack\n");
    return 1;
  }

#ifdef _WIN32
  if (WSAGetLastError() == WSAECONNRESET) {
    fprintf(stderr, "server aborted transfer after ready ack\n");
    return 1;
  }
#else
  if (errno == ECONNRESET) {
    fprintf(stderr, "server aborted transfer after ready ack\n");
    return 1;
  }
#endif

  sock_perror("recv_all(file_transfer_final_ack)");
  return 1;
}

static int client_connect(const char *ip, uint16_t port, socket_t *sock_out) {
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

#ifdef _WIN32
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
#else
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#endif
    sock_perror("connect");
    socket_close(sock);
    return 1;
  }

  *sock_out = sock;
  return 0;
}

static int client_get_file_size(int in, uint64_t *content_size_out) {
#ifdef _WIN32
  struct _stat64 st;
#else
  struct stat st;
#endif

  if (content_size_out == NULL) {
    fprintf(stderr, "invalid file size output\n");
    return 1;
  }

#ifdef _WIN32
  if (_fstat64(in, &st) != 0) {
    perror("_fstat64");
#else
  if (fstat(in, &st) != 0) {
    perror("fstat");
#endif
    return 1;
  }

  if (st.st_size < 0) {
    fprintf(stderr, "invalid source file size\n");
    return 1;
  }

  *content_size_out = (uint64_t)st.st_size;
  return 0;
}

static int client_rewind_input_file(int in) {
  if (fs_seek_start(in) != 0) {
    perror("lseek");
    return 1;
  }
  return 0;
}

static int client_measure_compressed_body(int in, uint64_t content_size,
                                          uint64_t *wire_body_size_out) {
  int exit_code = 1;
  char *raw_buf = NULL;
  char *cmp_buf = NULL;
  uint64_t remaining = content_size;
  uint64_t wire_body_size = 0;
  int cmp_cap = 0;

  if (wire_body_size_out == NULL) {
    fprintf(stderr, "invalid compression outputs\n");
    return 1;
  }

  *wire_body_size_out = 0;

  raw_buf = (char *)malloc((size_t)CHUNK_SIZE);
  if (raw_buf == NULL) {
    perror("malloc(raw_buf)");
    goto CLEANUP;
  }

  cmp_cap = LZ4_compressBound((int)CHUNK_SIZE);
  if (cmp_cap <= 0) {
    fprintf(stderr, "failed to size compression buffer\n");
    goto CLEANUP;
  }

  cmp_buf = (char *)malloc((size_t)cmp_cap);
  if (cmp_buf == NULL) {
    perror("malloc(cmp_buf)");
    goto CLEANUP;
  }

  while (remaining > 0) {
    uint32_t raw_size = 0;
    uint32_t stored_size = 0;
    size_t want = (size_t)CHUNK_SIZE;

    if ((uint64_t)want > remaining) {
      want = (size_t)remaining;
    }

    ssize_t nr = fs_read(in, raw_buf, want);
    if (nr < 0) {
      perror("read");
      goto CLEANUP;
    }
    if (nr == 0) {
      fprintf(stderr, "source file changed during compression\n");
      goto CLEANUP;
    }

    raw_size = (uint32_t)nr;
    stored_size = raw_size;

    int cmp_n = LZ4_compress_default(raw_buf, cmp_buf, (int)raw_size, cmp_cap);
    if (cmp_n > 0 && cmp_n < (int)raw_size) {
      stored_size = (uint32_t)cmp_n;
    }

    if (UINT64_MAX - wire_body_size <
        (uint64_t)proto_compressed_block_size(stored_size)) {
      fprintf(stderr, "compressed payload is too large\n");
      goto CLEANUP;
    }

    wire_body_size += (uint64_t)proto_compressed_block_size(stored_size);
    remaining -= raw_size;
  }
  *wire_body_size_out = wire_body_size;
  exit_code = 0;

CLEANUP:
  if (cmp_buf != NULL) free(cmp_buf);
  if (raw_buf != NULL) free(raw_buf);
  return exit_code;
}

static int client_send_compressed_body(int in, uint64_t content_size,
                                       socket_t sock) {
  int exit_code = 1;
  char *raw_buf = NULL;
  char *cmp_buf = NULL;
  uint64_t remaining = content_size;
  int cmp_cap = 0;

  raw_buf = (char *)malloc((size_t)CHUNK_SIZE);
  if (raw_buf == NULL) {
    perror("malloc(raw_buf)");
    goto CLEANUP;
  }

  cmp_cap = LZ4_compressBound((int)CHUNK_SIZE);
  if (cmp_cap <= 0) {
    fprintf(stderr, "failed to size compression buffer\n");
    goto CLEANUP;
  }

  cmp_buf = (char *)malloc((size_t)cmp_cap);
  if (cmp_buf == NULL) {
    perror("malloc(cmp_buf)");
    goto CLEANUP;
  }

  while (remaining > 0) {
    uint8_t block_header[HF_COMPRESS_BLOCK_HEADER_SIZE];
    uint8_t block_type = HF_COMPRESS_BLOCK_TYPE_RAW;
    const char *stored_buf = raw_buf;
    uint32_t raw_size = 0;
    uint32_t stored_size = 0;
    size_t want = (size_t)CHUNK_SIZE;
    if ((uint64_t)want > remaining) {
      want = (size_t)remaining;
    }

    ssize_t nr = fs_read(in, raw_buf, want);
    if (nr < 0) {
      perror("read");
      goto CLEANUP;
    }
    if (nr == 0) {
      fprintf(stderr, "source file changed during compression\n");
      goto CLEANUP;
    }

    raw_size = (uint32_t)nr;
    stored_size = raw_size;

    int cmp_n = LZ4_compress_default(raw_buf, cmp_buf, (int)raw_size, cmp_cap);
    if (cmp_n > 0 && cmp_n < (int)raw_size) {
      block_type = HF_COMPRESS_BLOCK_TYPE_LZ4;
      stored_buf = cmp_buf;
      stored_size = (uint32_t)cmp_n;
    }

    if (proto_encode_compressed_block_header(block_header, block_type, raw_size,
                                             stored_size) != PROTOCOL_OK) {
      fprintf(stderr, "failed to encode compressed block header\n");
      goto CLEANUP;
    }

    ssize_t sent = send_all(sock, block_header, sizeof(block_header));
    if (sent != (ssize_t)sizeof(block_header)) {
      sock_perror("send(compressed_block_header)");
      goto CLEANUP;
    }

    sent = send_all(sock, stored_buf, stored_size);
    if (sent != (ssize_t)stored_size) {
      sock_perror("send(compressed_block_data)");
      goto CLEANUP;
    }

    remaining -= raw_size;
  }

  exit_code = 0;

CLEANUP:
  if (cmp_buf != NULL) free(cmp_buf);
  if (raw_buf != NULL) free(raw_buf);
  return exit_code;
}

static int client_send_file_raw_body_buffered(int in,
                                              socket_t sock,
                                              uint64_t content_size) {
  int exit_code = 1;
  char *buf = (char *)malloc(CHUNK_SIZE);
  if (buf == NULL) {
    perror("malloc(buf)");
    return 1;
  }

  size_t pos = 0;
  uint64_t remaining = content_size;
  for (;;) {
    while (pos < CHUNK_SIZE && remaining > 0) {
      size_t want = CHUNK_SIZE - pos;
      if ((uint64_t)want > remaining)
        want = (size_t)remaining;

      ssize_t nr = fs_read(in, buf + pos, want);
      if (nr < 0) {
        perror("read");
        goto CLEANUP;
      }
      if (nr == 0) {
        fprintf(stderr, "source file changed during transfer\n");
        goto CLEANUP;
      }
      pos += (size_t)nr;
      remaining -= (uint64_t)nr;
    }

    if (pos > 0) {
      ssize_t sent = send_all(sock, buf, pos);
      if (sent != (ssize_t)pos) {
        sock_perror("send");
        goto CLEANUP;
      }
      pos = 0;
    }

    if (remaining == 0) {
      break;
    }
  }

  exit_code = 0;

CLEANUP:
  if (buf != NULL) free(buf);
  return exit_code;
}

static int client_send_file_raw_body(int in, socket_t sock, uint64_t content_size) {
  net_send_file_result_t send_file_res =
    net_send_file_all(sock, in, content_size);
  if (send_file_res == NET_SEND_FILE_OK) {
    return 0;
  }

  if (send_file_res == NET_SEND_FILE_UNSUPPORTED) {
    if (client_rewind_input_file(in) != 0) {
      return 1;
    }
    return client_send_file_raw_body_buffered(in, sock, content_size);
  }

  if (send_file_res == NET_SEND_FILE_SOURCE_CHANGED) {
    fprintf(stderr, "source file changed during transfer\n");
  } else if (send_file_res == NET_SEND_FILE_INVALID_ARGUMENT) {
    fprintf(stderr, "invalid raw transfer arguments\n");
  } else {
    sock_perror("sendfile");
  }

  return 1;
}

static int client_send_file_raw(const client_opt_t *opt) {
  int exit_code = 0;

  int in = -1;
  socket_t sock;
  socket_init(&sock);
  const char *path = opt->path;
  const char *file_name = NULL;
  uint16_t file_name_len = 0;
  uint64_t content_size = 0;

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

#ifdef _WIN32
  in = fs_open(path, O_RDONLY | O_BINARY, 0);
#else
  in = fs_open(path, O_RDONLY, 0);
#endif
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

  if (client_get_file_size(in, &content_size) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_connect(opt->ip, opt->port, &sock) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  uint64_t payload_size =
    (uint64_t)proto_file_transfer_prefix_size(file_name_len) + content_size;

  protocol_header_t header = {0};
  init_header(&header);
  header.msg_type = opt->msg_type;
  header.flags = opt->msg_flags;
  header.payload_size = payload_size;

  uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
  protocol_result_t proto_res = encode_header(&header, header_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode protocol header\n");
    exit_code = 1;
    goto CLEANUP;
  }

  proto_res = send_header(sock, header_buf);
  if (proto_res != PROTOCOL_OK) {
    sock_perror("send_header");
    exit_code = 1;
    goto CLEANUP;
  }

  proto_res = proto_send_file_transfer_prefix(sock, file_name, content_size);
  if (proto_res != PROTOCOL_OK) {
    if (proto_res == PROTOCOL_ERR_FILE_NAME_LEN) {
      fprintf(stderr, "invalid file name length\n");
    } else {
      sock_perror("protocol_send_file_transfer_prefix");
    }
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_recv_ack(sock, "recv_all(file_transfer_ready_ack)",
                      "server reported transfer failure") != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_send_file_raw_body(in, sock, content_size) != 0) {
    exit_code = 1;
    goto CLEANUP;
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

  if (client_recv_file_final_result(sock) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

CLEANUP:
  socket_close(sock);
  if (in != -1) fs_close(in);

  return exit_code;
}

static int client_send_file_compressed(const client_opt_t *opt) {
  int exit_code = 0;

  int in = -1;
  socket_t sock;
  socket_init(&sock);
  const char *path = opt->path;
  const char *file_name = NULL;
  uint16_t file_name_len = 0;
  uint64_t content_size = 0;
  uint64_t wire_body_size = 0;

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

#ifdef _WIN32
  in = fs_open(path, O_RDONLY | O_BINARY, 0);
#else
  in = fs_open(path, O_RDONLY, 0);
#endif
  if (in == -1) {
    perror("open");
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_get_file_size(in, &content_size) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_measure_compressed_body(in, content_size, &wire_body_size) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_rewind_input_file(in) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_connect(opt->ip, opt->port, &sock) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  uint64_t payload_size =
    (uint64_t)proto_file_transfer_prefix_size(file_name_len) + wire_body_size;

  protocol_header_t header = {0};
  init_header(&header);
  header.msg_type = opt->msg_type;
  header.flags = opt->msg_flags;
  header.payload_size = payload_size;

  uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
  protocol_result_t proto_res = encode_header(&header, header_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode protocol header\n");
    exit_code = 1;
    goto CLEANUP;
  }

  proto_res = send_header(sock, header_buf);
  if (proto_res != PROTOCOL_OK) {
    sock_perror("send_header");
    exit_code = 1;
    goto CLEANUP;
  }

  proto_res = proto_send_file_transfer_prefix(sock, file_name, content_size);
  if (proto_res != PROTOCOL_OK) {
    if (proto_res == PROTOCOL_ERR_FILE_NAME_LEN) {
      fprintf(stderr, "invalid file name length\n");
    } else {
      sock_perror("protocol_send_file_transfer_prefix");
    }
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_recv_ack(sock, "recv_all(file_transfer_ready_ack)",
                      "server reported transfer failure") != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_send_compressed_body(in, content_size, sock) != 0) {
    exit_code = 1;
    goto CLEANUP;
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

  if (client_recv_file_final_result(sock) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

CLEANUP:
  socket_close(sock);
  if (in != -1) fs_close(in);

  return exit_code;
}

static int client_send_message(const client_opt_t *opt) {
  int exit_code = 0;
  socket_t sock;
  socket_init(&sock);
  const char *message = opt->message;
  size_t message_len = 0;

  message_len = strlen(message);
  if (message_len > HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE) {
    fprintf(stderr, "message too large\n");
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_connect(opt->ip, opt->port, &sock) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  uint64_t payload_size = (uint64_t)message_len;

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

  proto_res = send_header(sock, header_buf);
  if (proto_res != PROTOCOL_OK) {
    sock_perror("send_header");
    exit_code = 1;
    goto CLEANUP;
  }

  if (message_len > 0) {
    ssize_t sent = send_all(sock, message, message_len);
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

  if (client_recv_ack(sock, "recv_all(message_ack)",
                      "server reported transfer failure") != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

CLEANUP:
  socket_close(sock);

  return exit_code;
}

int client(const client_opt_t *cli_opt) {
  if (cli_opt == NULL) {
    fprintf(stderr, "invalid client options\n");
    return 1;
  }

  switch (cli_opt->msg_type) {
    case HF_MSG_TYPE_FILE_TRANSFER:
      if ((cli_opt->msg_flags & HF_MSG_FLAG_COMPRESS) != 0) {
        return client_send_file_compressed(cli_opt);
      }
      return client_send_file_raw(cli_opt);
    case HF_MSG_TYPE_TEXT_MESSAGE:
      return client_send_message(cli_opt);
    default:
      fprintf(stderr, "unsupported client message type: %u\n",
              (unsigned)cli_opt->msg_type);
      return 1;
  }
}
