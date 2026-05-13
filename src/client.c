#include "client.h"

#include "discovery.h"
#include "fs.h"
#include "net.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

#define CLIENT_CONTROL_SOCKET_TIMEOUT_MS 30000u

static const char *client_protocol_result_name(protocol_result_t res) {
  switch (res) {
    case PROTOCOL_OK:
      return "ok";
    case PROTOCOL_ERR_HEADER_MAGIC:
      return "header magic";
    case PROTOCOL_ERR_HEADER_VERSION:
      return "header version";
    case PROTOCOL_ERR_HEADER_MSG_TYPE:
      return "header msg type";
    case PROTOCOL_ERR_HEADER_MSG_FLAG:
      return "header msg flag";
    case PROTOCOL_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case PROTOCOL_ERR_FILE_NAME_LEN:
      return "file name length";
    case PROTOCOL_ERR_INVALID_FILE_NAME:
      return "invalid file name";
    case PROTOCOL_ERR_PAYLOAD_SIZE_MISMATCH:
      return "payload size mismatch";
    case PROTOCOL_ERR_IO:
      return "io";
    case PROTOCOL_ERR_SHORT_WRITE:
      return "short write";
    case PROTOCOL_ERR_ALLOC:
      return "alloc";
    case PROTOCOL_ERR_EOF:
      return "unexpected eof";
    default:
      return "unknown";
  }
}

static int client_recv_response(socket_t sock,
                                uint8_t expected_phase,
                                const char *kind,
                                res_frame_t *frame_out) {
  res_frame_t frame = {0};
  protocol_result_t res = recv_res_frame(sock, &frame);

  if (res != PROTOCOL_OK) {
    if (res == PROTOCOL_ERR_EOF) {
      fprintf(stderr, "server closed connection while waiting for %s response\n",
              kind);
    } else if (res == PROTOCOL_ERR_IO) {
      sock_perror("recv_res_frame");
    } else {
      fprintf(stderr, "invalid %s response frame: %s\n",
              kind,
              client_protocol_result_name(res));
    }
    return 1;
  }

  if (frame.phase != expected_phase) {
    fprintf(stderr, "unexpected %s response phase: got=%u expected=%u\n",
            kind, (unsigned)frame.phase, (unsigned)expected_phase);
    return 1;
  }

  if (frame.status == PROTO_STATUS_OK) {
    if (frame.error_code != PROTOCOL_OK) {
      fprintf(stderr, "invalid success response error_code=%u\n",
              (unsigned)frame.error_code);
      return 1;
    }
  } else if (frame.error_code == PROTOCOL_OK) {
    fprintf(stderr, "missing %s failure reason for phase=%u status=%u\n",
            kind, (unsigned)frame.phase, (unsigned)frame.status);
    return 1;
  }

  if (frame_out != NULL) {
    *frame_out = frame;
  }
  return 0;
}

static int client_check_response(const res_frame_t *frame,
                                 uint8_t expected_phase,
                                 const char *kind) {
  if (frame == NULL) {
    fprintf(stderr, "invalid %s response\n", kind);
    return 1;
  }

  if (frame->status == PROTO_STATUS_OK) {
    return 0;
  }

  if (expected_phase == PROTO_PHASE_READY) {
    if (frame->status == PROTO_STATUS_REJECTED) {
      fprintf(stderr, "server reported %s failure: %s\n",
              kind,
              client_protocol_result_name((protocol_result_t)frame->error_code));
      return 1;
    }

    fprintf(stderr, "invalid %s ready response status=%u error=%s\n",
            kind,
            (unsigned)frame->status,
            client_protocol_result_name((protocol_result_t)frame->error_code));
    return 1;
  }

  if (expected_phase == PROTO_PHASE_FINAL) {
    if (frame->status == PROTO_STATUS_FAILED) {
      fprintf(stderr, "server reported %s failure: %s\n",
              kind,
              client_protocol_result_name((protocol_result_t)frame->error_code));
      return 1;
    }

    fprintf(stderr, "invalid %s final response status=%u error=%s\n",
            kind,
            (unsigned)frame->status,
            client_protocol_result_name((protocol_result_t)frame->error_code));
    return 1;
  }

  fprintf(stderr, "invalid expected %s phase=%u\n", kind,
          (unsigned)expected_phase);
  return 1;
}

static int client_recv_checked_response(socket_t sock,
                                        uint8_t expected_phase,
                                        const char *kind,
                                        res_frame_t *frame_out) {
  res_frame_t frame = {0};

  if (client_recv_response(sock, expected_phase, kind, &frame) != 0) {
    return 1;
  }
  if (client_check_response(&frame, expected_phase, kind) != 0) {
    return 1;
  }
  if (frame_out != NULL) {
    *frame_out = frame;
  }
  return 0;
}

static int client_send_header_payload(socket_t sock,
                                      uint8_t msg_type,
                                      uint64_t payload_size,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      const char *send_ctx) {
  protocol_header_t header = {0};
  uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
  uint8_t preamble_buf[HF_PROTOCOL_HEADER_SIZE + sizeof(uint16_t) +
                       HF_PROTOCOL_MAX_FILE_NAME_LEN + sizeof(uint64_t)];
  protocol_result_t proto_res = PROTOCOL_OK;

  init_header(&header);
  header.msg_type = msg_type;
  header.flags = HF_MSG_FLAG_NONE;
  header.payload_size = payload_size;

  proto_res = encode_header(&header, header_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode protocol header\n");
    return 1;
  }

  if (payload_len <= sizeof(preamble_buf) - sizeof(header_buf)) {
    memcpy(preamble_buf, header_buf, sizeof(header_buf));
    if (payload_len > 0) {
      memcpy(preamble_buf + sizeof(header_buf), payload, payload_len);
    }
    proto_res = proto_send_payload(sock, preamble_buf,
                                   sizeof(header_buf) + payload_len);
    if (proto_res != PROTOCOL_OK) {
      sock_perror(send_ctx);
      return 1;
    }
    return 0;
  }

  proto_res = proto_send_payload(sock, header_buf, sizeof(header_buf));
  if (proto_res != PROTOCOL_OK) {
    sock_perror(send_ctx);
    return 1;
  }
  if (payload_len > 0 &&
      proto_send_payload(sock, payload, payload_len) != PROTOCOL_OK) {
    sock_perror(send_ctx);
    return 1;
  }

  return 0;
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

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    sock_perror("connect");
    socket_close(sock);
    return 1;
  }

  if (net_set_socket_timeouts(sock, CLIENT_CONTROL_SOCKET_TIMEOUT_MS) != 0) {
    sock_perror("setsockopt(client_timeout)");
    socket_close(sock);
    return 1;
  }

  *sock_out = sock;
  return 0;
}

static int client_get_file_size(int in, uint64_t *content_size_out) {
  struct stat st;

  if (content_size_out == NULL) {
    fprintf(stderr, "invalid file size output\n");
    return 1;
  }

  if (fstat(in, &st) != 0) {
    perror("fstat");
    return 1;
  }

  if (!S_ISREG(st.st_mode) || st.st_size < 0) {
    fprintf(stderr, "invalid source file\n");
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

  in = fs_open(path, O_RDONLY, 0);
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

  char discovered_ip[64];
  const char *connect_ip = opt->ip;
  uint16_t connect_port = opt->port;

  if (connect_ip == NULL) {
    uint16_t discovery_port = (opt->port < 65535u) ? (uint16_t)(opt->port + 1u) : opt->port;
    if (discovery_client_find(discovery_port, discovered_ip,
                               sizeof(discovered_ip), &connect_port) != 0) {
      fprintf(stderr, "no HFile server found on LAN\n");
      exit_code = 1;
      goto CLEAN_UP;
    }
    connect_ip = discovered_ip;
    fprintf(stdout, "discovered server at %s:%u\n", connect_ip,
            (unsigned)connect_port);
  }

  if (client_connect(connect_ip, connect_port, &sock) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  uint64_t payload_size =
    (uint64_t)proto_file_transfer_prefix_size(file_name_len) + content_size;

  uint8_t file_prefix_buf[sizeof(uint16_t) + HF_PROTOCOL_MAX_FILE_NAME_LEN + sizeof(uint64_t)];
  protocol_result_t proto_res = encode_file_prefix(file_name, content_size,
                                                    file_prefix_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode file_prefix\n");
    exit_code = 1;
    goto CLEAN_UP;
  }


  size_t file_prefix_size = proto_file_transfer_prefix_size(file_name_len);
  if (client_send_header_payload(sock, HF_MSG_TYPE_SEND_FILE, payload_size,
                                 file_prefix_buf, file_prefix_size,
                                 "send(file_preamble)") != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  res_frame_t r_f = {0};
  if (client_recv_checked_response(sock, PROTO_PHASE_READY, "transfer", &r_f) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (net_set_socket_timeouts(
        sock, net_transfer_timeout_ms(content_size)) != 0) {
    sock_perror("setsockopt(client_transfer_timeout)");
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (client_send_file_body(in, sock, content_size) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (shutdown(sock, SHUT_WR) < 0) {
    sock_perror("shutdown(SHUT_WR)");
  }
  if (client_recv_checked_response(sock, PROTO_PHASE_FINAL, "transfer", &r_f) != 0) {
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

int client(const client_opt_t *cli_opt) {
  if (cli_opt == NULL) {
    fprintf(stderr, "invalid client options\n");
    return 1;
  }

  return client_send_file_raw(cli_opt);
}
