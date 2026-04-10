#include "client.h"

#include "fs.h"
#include "net.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <sys/stat.h>

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
  socket_t sock;

  struct sockaddr_in addr;

  socket_init(&sock);

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (is_socket_invalid(sock)) {
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

static int client_send_file_body(int in, socket_t sock, uint64_t content_size) {
  net_send_file_result_t send_file_res =
    net_send_file_best_effort(sock, in, content_size);
  if (send_file_res == NET_SEND_FILE_OK) {
    return 0;
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
    goto CLEAN_UP;
  }

  if (proto_get_file_name_len(file_name, &file_name_len) != 0) {
    fprintf(stderr, "invalid file name length\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

#ifdef _WIN32
  in = fs_open(path, O_RDONLY | O_BINARY, 0);
#else
  in = fs_open(path, O_RDONLY, 0);
#endif
  if (in == -1) {
    perror("open");
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (proto_file_transfer_prefix_size(file_name_len) > CHUNK_SIZE) {
    fprintf(stderr, "protocol payload prefix exceeds buffer size\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (client_get_file_size(in, &content_size) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }
  
  if (content_size > HF_MAX_FILE_SIZE) {
    fprintf(stderr, "MAX_FILE_SIZE is 100GB\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (client_connect(opt->ip, opt->port, &sock) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  uint64_t payload_size =
    (uint64_t)proto_file_transfer_prefix_size(file_name_len) + content_size;

  protocol_header_t header = {0};
  init_header(&header);
  header.msg_type = opt->msg_type;
  header.flags = HF_MSG_FLAG_NONE;
  header.payload_size = payload_size;

  uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
  protocol_result_t proto_res = encode_header(&header, header_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode protocol header\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  proto_res = send_header(sock, header_buf);
  if (proto_res != PROTOCOL_OK) {
    sock_perror("send_header");
    exit_code = 1;
    goto CLEAN_UP;
  }

  proto_res = proto_send_file_transfer_prefix(sock, file_name, content_size);
  if (proto_res != PROTOCOL_OK) {
    if (proto_res == PROTOCOL_ERR_FILE_NAME_LEN) {
      fprintf(stderr, "invalid file name length\n");
    } else {
      fprintf(stderr, "failed to send file transfer prefix\n");
      sock_perror("protocol_send_file_transfer_prefix");
    }
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (client_recv_ack(sock, "recv_all(file_transfer_ready_ack)",
                      "server reported transfer failure") != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (client_send_file_body(in, sock, content_size) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
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
    goto CLEAN_UP;
  }

CLEAN_UP:
  socket_close(sock);
  if (in != -1) {
    fs_close(in);
  }

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
    goto CLEAN_UP;
  }

  if (client_connect(opt->ip, opt->port, &sock) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  protocol_header_t header = {0};
  init_header(&header);
  header.msg_type = HF_MSG_TYPE_TEXT_MESSAGE;
  header.flags = HF_MSG_FLAG_NONE;
  header.payload_size = (uint64_t)message_len;

  uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
  protocol_result_t proto_res = encode_header(&header, header_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode protocol header\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  proto_res = send_header(sock, header_buf);
  if (proto_res != PROTOCOL_OK) {
    sock_perror("send_header");
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (message_len > 0) {
    ssize_t sent = send_all(sock, message, message_len);
    if (sent != (ssize_t)message_len) {
      sock_perror("send(message)");
      exit_code = 1;
      goto CLEAN_UP;
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
    goto CLEAN_UP;
  }

CLEAN_UP:
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
      return client_send_file_raw(cli_opt);
    case HF_MSG_TYPE_TEXT_MESSAGE:
      return client_send_message(cli_opt);
    default:
      fprintf(stderr, "unsupported client message type: %u\n",
              (unsigned)cli_opt->msg_type);
      return 1;
  }
}