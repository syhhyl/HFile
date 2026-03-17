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
#include <errno.h>
#include "fs.h"
#include <sys/stat.h>
#include "lz4.h"


static int client_recv_ack(socket_t sock, uint64_t *perf_net_ns,
                           const char *recv_ctx,
                           const char *failure_ctx) {
  uint8_t ack = 1;
  uint64_t t_ack_start = now_ns();
  if (recv_all(sock, &ack, sizeof(ack)) != (ssize_t)sizeof(ack)) {
    *perf_net_ns += now_ns() - t_ack_start;
    sock_perror(recv_ctx);
    return 1;
  }
  *perf_net_ns += now_ns() - t_ack_start;
  if (ack != 0) {
    fprintf(stderr, "%s (ack=%u)\n", failure_ctx, (unsigned)ack);
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

static int client_get_file_size(int in, uint64_t *content_size_out,
                                uint64_t *perf_io_ns) {
#ifdef _WIN32
  struct _stat64 st;
#else
  struct stat st;
#endif
  uint64_t t_stat_start = now_ns();

  if (content_size_out == NULL || perf_io_ns == NULL) {
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
    *perf_io_ns += now_ns() - t_stat_start;
    return 1;
  }

  *perf_io_ns += now_ns() - t_stat_start;
  if (st.st_size < 0) {
    fprintf(stderr, "invalid source file size\n");
    return 1;
  }

  *content_size_out = (uint64_t)st.st_size;
  return 0;
}

static int client_tmpfile_write_all(FILE *fp, const void *buf, size_t len,
                                    uint64_t *perf_io_ns) {
  const unsigned char *p = (const unsigned char *)buf;
  size_t total = 0;

  if (fp == NULL || perf_io_ns == NULL) {
    fprintf(stderr, "invalid temporary file write arguments\n");
    return 1;
  }

  while (total < len) {
    uint64_t t_write_start = now_ns();
    size_t nw = fwrite(p + total, 1, len - total, fp);
    *perf_io_ns += now_ns() - t_write_start;
    if (nw == 0) {
      perror("fwrite(tmpfile)");
      return 1;
    }
    total += nw;
  }

  return 0;
}

static int client_prepare_compressed_body(int in, uint64_t content_size,
                                          FILE **body_fp_out,
                                          uint64_t *wire_body_size_out,
                                          uint64_t *perf_io_ns) {
  int exit_code = 1;
  FILE *body_fp = NULL;
  char *raw_buf = NULL;
  char *cmp_buf = NULL;
  uint64_t remaining = content_size;
  uint64_t wire_body_size = 0;
  int cmp_cap = 0;

  if (body_fp_out == NULL || wire_body_size_out == NULL || perf_io_ns == NULL) {
    fprintf(stderr, "invalid compression outputs\n");
    return 1;
  }

  *body_fp_out = NULL;
  *wire_body_size_out = 0;

  body_fp = tmpfile();
  if (body_fp == NULL) {
    perror("tmpfile");
    goto CLEANUP;
  }

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

    uint64_t t_read_start = now_ns();
    ssize_t nr = fs_read(in, raw_buf, want);
    *perf_io_ns += now_ns() - t_read_start;
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

    if (client_tmpfile_write_all(body_fp, block_header,
                                 sizeof(block_header), perf_io_ns) != 0) {
      goto CLEANUP;
    }
    if (client_tmpfile_write_all(body_fp, stored_buf, stored_size,
                                 perf_io_ns) != 0) {
      goto CLEANUP;
    }

    if (UINT64_MAX - wire_body_size <
        (uint64_t)proto_compressed_block_size(stored_size)) {
      fprintf(stderr, "compressed payload is too large\n");
      goto CLEANUP;
    }

    wire_body_size += (uint64_t)proto_compressed_block_size(stored_size);
    remaining -= raw_size;
  }

  uint64_t t_flush_start = now_ns();
  if (fflush(body_fp) != 0) {
    *perf_io_ns += now_ns() - t_flush_start;
    perror("fflush(tmpfile)");
    goto CLEANUP;
  }
  *perf_io_ns += now_ns() - t_flush_start;

  uint64_t t_seek_start = now_ns();
  if (fseek(body_fp, 0, SEEK_SET) != 0) {
    *perf_io_ns += now_ns() - t_seek_start;
    perror("fseek(tmpfile)");
    goto CLEANUP;
  }
  *perf_io_ns += now_ns() - t_seek_start;

  *body_fp_out = body_fp;
  *wire_body_size_out = wire_body_size;
  body_fp = NULL;
  exit_code = 0;

CLEANUP:
  if (cmp_buf != NULL) free(cmp_buf);
  if (raw_buf != NULL) free(raw_buf);
  if (body_fp != NULL) fclose(body_fp);
  return exit_code;
}

static int client_send_tmpfile_body(FILE *body_fp, socket_t sock,
                                    uint64_t body_size,
                                    uint64_t *perf_io_ns,
                                    uint64_t *perf_net_ns) {
  int exit_code = 1;
  char *buf = NULL;
  uint64_t remaining = body_size;

  if (body_fp == NULL || perf_io_ns == NULL || perf_net_ns == NULL) {
    fprintf(stderr, "invalid temporary body sender arguments\n");
    return 1;
  }

  buf = (char *)malloc((size_t)CHUNK_SIZE);
  if (buf == NULL) {
    perror("malloc(buf)");
    goto CLEANUP;
  }

  while (remaining > 0) {
    size_t want = (size_t)CHUNK_SIZE;
    if ((uint64_t)want > remaining) {
      want = (size_t)remaining;
    }

    uint64_t t_read_start = now_ns();
    size_t nr = fread(buf, 1, want, body_fp);
    *perf_io_ns += now_ns() - t_read_start;
    if (nr != want) {
      if (ferror(body_fp)) {
        perror("fread(tmpfile)");
      } else {
        fprintf(stderr, "temporary compressed body truncated\n");
      }
      goto CLEANUP;
    }

    uint64_t t_send_start = now_ns();
    ssize_t sent = send_all(sock, buf, nr);
    *perf_net_ns += now_ns() - t_send_start;
    if (sent != (ssize_t)nr) {
      sock_perror("send");
      goto CLEANUP;
    }

    remaining -= (uint64_t)nr;
  }

  exit_code = 0;

CLEANUP:
  if (buf != NULL) free(buf);
  return exit_code;
}

static int client_send_plain_file_transfer(const client_opt_t *opt) {
  int exit_code = 0;

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

  if (client_get_file_size(in, &content_size, &perf_io_ns) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_connect(opt->ip, opt->port, &sock, &perf_net_ns) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

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

  if (client_recv_ack(sock, &perf_net_ns, "recv_all(file_transfer_ready_ack)",
                      "server reported transfer failure") != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  buf = (char *)malloc(CHUNK_SIZE);
  if (buf == NULL) {
    perror("malloc(buf)");
    exit_code = 1;
    goto CLEANUP;
  }

  size_t pos = 0;
  uint64_t remaining = content_size;
  for (;;) {
    while (pos < CHUNK_SIZE && remaining > 0) {
      size_t want = CHUNK_SIZE - pos;
      if ((uint64_t)want > remaining)
        want = (size_t)remaining;

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

    //TODO can we use multithreading ?
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

  if (client_recv_ack(sock, &perf_net_ns, "recv_all(file_transfer_final_ack)",
                      "server reported transfer failure") != 0) {
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

static int client_send_compressed_file_transfer(const client_opt_t *opt) {
  int exit_code = 0;

  uint64_t perf_start_ns = now_ns();
  uint64_t perf_io_ns = 0;
  uint64_t perf_net_ns = 0;
  uint64_t perf_file_bytes = 0;
  uint64_t perf_wire_bytes = 0;

  int in = -1;
  FILE *body_fp = NULL;
#ifdef _WIN32
  socket_t sock = INVALID_SOCKET;
#else
  socket_t sock = -1;
#endif
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

  if (client_get_file_size(in, &content_size, &perf_io_ns) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_prepare_compressed_body(in, content_size, &body_fp, &wire_body_size,
                                     &perf_io_ns) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  if (in != -1) {
    fs_close(in);
    in = -1;
  }

  if (wire_body_size >= content_size) {
    fclose(body_fp);
    return client_send_plain_file_transfer(opt);
  }

  if (client_connect(opt->ip, opt->port, &sock, &perf_net_ns) != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  perf_file_bytes = content_size;
  uint64_t payload_size =
    (uint64_t)proto_file_transfer_prefix_size(file_name_len) + wire_body_size;
  perf_wire_bytes = (uint64_t)HF_PROTOCOL_HEADER_SIZE + payload_size;

  protocol_header_t header = {0};
  init_header(&header);
  header.msg_type = HF_MSG_TYPE_FILE_TRANSFER_COMPRESSED;
  header.flags = HF_MSG_FLAG_NONE;
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

  if (client_recv_ack(sock, &perf_net_ns, "recv_all(file_transfer_ready_ack)",
                      "server reported transfer failure") != 0) {
    exit_code = 1;
    goto CLEANUP;
  }

  if (client_send_tmpfile_body(body_fp, sock, wire_body_size, &perf_io_ns,
                               &perf_net_ns) != 0) {
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

  if (client_recv_ack(sock, &perf_net_ns, "recv_all(file_transfer_final_ack)",
                      "server reported transfer failure") != 0) {
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
  if (body_fp != NULL) fclose(body_fp);
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

  if (client_recv_ack(sock, &perf_net_ns, "recv_all(message_ack)",
                      "server reported transfer failure") != 0) {
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
      if ((cli_opt->msg_flags & HF_MSG_FLAG_COMPRESS) != 0) {
        return client_send_compressed_file_transfer(cli_opt);
      }
      return client_send_plain_file_transfer(cli_opt);
    case HF_MSG_TYPE_TEXT_MESSAGE:
      return client_send_text_message(cli_opt);
    default:
      fprintf(stderr, "unsupported client message type: %u\n",
              (unsigned)cli_opt->msg_type);
      return 1;
  }
}
