#include "client.h"

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

#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif

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

static int client_open_temp_download(const char *final_path,
                                     char *tmp_path,
                                     size_t tmp_path_cap,
                                     int *fd_out) {
  int out = -1;

  if (final_path == NULL || tmp_path == NULL || fd_out == NULL) {
    return 1;
  }

#ifdef _WIN32
  int pid = _getpid();
#else
  int pid = (int)getpid();
#endif

  for (int attempt = 0; attempt < 3; attempt++) {
    if (fs_make_temp_path(tmp_path, tmp_path_cap, final_path, pid, attempt) != 0) {
      return 1;
    }
    out = fs_open_temp_file(tmp_path);
    if (out != -1) {
      *fd_out = out;
      return 0;
    }
    if (errno != EEXIST) {
      return 1;
    }
  }

  return 1;
}

static int client_recv_file_body(socket_t sock, int out_fd, uint64_t content_size) {
  net_recv_file_result_t recv_res =
    net_recv_file_best_effort(sock, out_fd, content_size);
  if (recv_res == NET_RECV_FILE_OK) {
    return 0;
  }

  if (recv_res == NET_RECV_FILE_EOF) {
    fprintf(stderr, "server closed connection while sending file body\n");
  } else if (recv_res == NET_RECV_FILE_INVALID_ARGUMENT) {
    fprintf(stderr, "invalid download arguments\n");
  } else {
    sock_perror("recv(file_body)");
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
  protocol_result_t proto_res;

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
  uint8_t preamble_buf[HF_PROTOCOL_HEADER_SIZE + sizeof(uint16_t) +
                       HF_PROTOCOL_MAX_FILE_NAME_LEN + sizeof(uint64_t)];
  proto_res = encode_header(&header, header_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode protocol header\n");
    exit_code = 1;
    goto CLEAN_UP;
  }
  
  uint8_t file_prefix_buf[sizeof(uint16_t) + HF_PROTOCOL_MAX_FILE_NAME_LEN + sizeof(uint64_t)];
  proto_res = encode_file_prefix(file_name, content_size, file_prefix_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode file_prefix\n");
    exit_code = 1;
    goto CLEAN_UP;
  }


  size_t file_prefix_size = proto_file_transfer_prefix_size(file_name_len);
  memcpy(preamble_buf, header_buf, sizeof(header_buf));
  memcpy(preamble_buf + sizeof(header_buf), file_prefix_buf, file_prefix_size);

  proto_res = proto_send_payload(sock, preamble_buf,
                                 sizeof(header_buf) + file_prefix_size);
  if (proto_res != PROTOCOL_OK) {
    sock_perror("send(file_preamble)");
    exit_code = 1;
    goto CLEAN_UP;
  }

  res_frame_t r_f = {0};
  if (client_recv_response(sock, PROTO_PHASE_READY, "transfer", &r_f) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (client_check_response(&r_f, PROTO_PHASE_READY, "transfer") != 0) {
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

  if (client_recv_response(sock, PROTO_PHASE_FINAL, "transfer", &r_f) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (client_check_response(&r_f, PROTO_PHASE_FINAL, "transfer") != 0) {
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

  res_frame_t response = {0};
  if (client_recv_response(sock, PROTO_PHASE_FINAL, "message", &response) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (client_check_response(&response, PROTO_PHASE_FINAL, "message") != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

CLEAN_UP:
  socket_close(sock);
  return exit_code;
}

static int client_get_file(const client_opt_t *opt) {
  int exit_code = 0;
  int out = -1;
  socket_t sock;
  socket_init(&sock);
  const char *remote_path = opt->remote_path;
  const char *output_path = opt->output_path;
  char *offered_name = NULL;
  uint64_t content_size = 0;
  uint16_t remote_name_len = 0;
  char tmp_path[4096];
  protocol_result_t proto_res = PROTOCOL_OK;

  tmp_path[0] = '\0';

  if (remote_path == NULL || fs_validate_file_name(remote_path) != 0) {
    fprintf(stderr, "invalid remote file\n");
    return 1;
  }
  if (proto_get_file_name_len(remote_path, &remote_name_len) != 0) {
    fprintf(stderr, "invalid remote file length\n");
    return 1;
  }

  if (client_connect(opt->ip, opt->port, &sock) != 0) {
    return 1;
  }

  {
    protocol_header_t header = {0};
    uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
    uint8_t request_buf[sizeof(uint16_t) + HF_PROTOCOL_MAX_FILE_NAME_LEN];
    uint8_t preamble_buf[HF_PROTOCOL_HEADER_SIZE + sizeof(uint16_t) +
                         HF_PROTOCOL_MAX_FILE_NAME_LEN];
    size_t request_size = proto_file_name_only_size(remote_name_len);

    init_header(&header);
    header.msg_type = HF_MSG_TYPE_GET_FILE;
    header.flags = HF_MSG_FLAG_NONE;
    header.payload_size = (uint64_t)proto_file_name_only_size(remote_name_len);

    proto_res = encode_header(&header, header_buf);
    if (proto_res != PROTOCOL_OK) {
      fprintf(stderr, "failed to encode protocol header\n");
      exit_code = 1;
      goto CLEAN_UP;
    }

    proto_res = encode_file_name_only(remote_path, request_buf);
    if (proto_res != PROTOCOL_OK) {
      fprintf(stderr, "failed to encode get request\n");
      exit_code = 1;
      goto CLEAN_UP;
    }

    memcpy(preamble_buf, header_buf, sizeof(header_buf));
    memcpy(preamble_buf + sizeof(header_buf), request_buf, request_size);

    proto_res = proto_send_payload(sock, preamble_buf,
                                   sizeof(header_buf) + request_size);
    if (proto_res != PROTOCOL_OK) {
      sock_perror("send(get_preamble)");
      exit_code = 1;
      goto CLEAN_UP;
    }
  }

  {
    res_frame_t response = {0};
    if (client_recv_response(sock, PROTO_PHASE_READY, "get", &response) != 0) {
      exit_code = 1;
      goto CLEAN_UP;
    }
    if (client_check_response(&response, PROTO_PHASE_READY, "get") != 0) {
      exit_code = 1;
      goto CLEAN_UP;
    }
  }

  proto_res = proto_recv_file_transfer_prefix(sock, &offered_name, &content_size);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to receive file header: %s\n",
            client_protocol_result_name(proto_res));
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (content_size > HF_MAX_FILE_SIZE) {
    fprintf(stderr, "MAX_FILE_SIZE is 100GB\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (output_path == NULL) {
    if (fs_validate_file_name(offered_name) != 0) {
      fprintf(stderr, "invalid offered file name\n");
      exit_code = 1;
      goto CLEAN_UP;
    }
    output_path = offered_name;
  }

  if (client_open_temp_download(output_path, tmp_path, sizeof(tmp_path), &out) != 0) {
    perror("open(temp_download)");
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (client_recv_file_body(sock, out, content_size) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (fs_close(out) != 0) {
    perror("close(temp_download)");
    out = -1;
    exit_code = 1;
    goto CLEAN_UP;
  }
  out = -1;

  {
    res_frame_t response = {0};
    if (client_recv_response(sock, PROTO_PHASE_FINAL, "get", &response) != 0) {
      exit_code = 1;
      goto CLEAN_UP;
    }
    if (client_check_response(&response, PROTO_PHASE_FINAL, "get") != 0) {
      exit_code = 1;
      goto CLEAN_UP;
    }
  }

  if (fs_finalize_temp_file(tmp_path, output_path, NULL) != 0) {
    perror("rename(download)");
    exit_code = 1;
    goto CLEAN_UP;
  }
  tmp_path[0] = '\0';

CLEAN_UP:
  if (out != -1) {
    fs_close(out);
  }
  if (tmp_path[0] != '\0') {
    fs_remove_quiet(tmp_path);
  }
  if (offered_name != NULL) {
    free(offered_name);
  }
  socket_close(sock);
  return exit_code;
}

int client(const client_opt_t *cli_opt) {
  if (cli_opt == NULL) {
    fprintf(stderr, "invalid client options\n");
    return 1;
  }

  switch (cli_opt->msg_type) {
    case HF_MSG_TYPE_SEND_FILE:
      return client_send_file_raw(cli_opt);
    case HF_MSG_TYPE_TEXT_MESSAGE:
      return client_send_message(cli_opt);
    case HF_MSG_TYPE_GET_FILE:
      return client_get_file(cli_opt);
    default:
      fprintf(stderr, "unsupported client message type: %u\n",
              (unsigned)cli_opt->msg_type);
      return 1;
  }
}
