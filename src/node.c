#include "discovery.h"
#include "fs.h"
#include "net.h"
#include "node.h"
#include "protocol.h"

#include <stddef.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

static protocol_result_t node_recv_file(socket_t conn, const char *path,
                                        const protocol_header_t *proto_header);

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

  if (listen(sock, 1) == -1) {
    sock_perror("listen");
    socket_close(sock);
    return 1;
  }

  *sock_out = sock;
  return 0;
}

static int node_handle_connection(socket_t conn, const char *path) {
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
      return node_recv_file(conn, path, &proto_header) == PROTOCOL_OK ? 0 : 1;
  }

  return 1;
}

static protocol_result_t node_send_response(
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

static protocol_result_t node_recv_socket_file(socket_t conn,
                                               const char *base_dir,
                                               const char *file_name,
                                               uint64_t content_size) {
  int fd = -1;
  protocol_result_t result = PROTOCOL_ERR_IO;
  char full_path[4096];
  char tmp_path[4096];

  if (base_dir == NULL || file_name == NULL) {
    fprintf(stderr, "invalid receive file args\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (fs_join_relative_path(full_path, sizeof(full_path), base_dir, file_name) != 0) {
    fprintf(stderr, "output path is too long\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  for (int attempt = 0; attempt < 3 && fd < 0; attempt++) {
    if (fs_build_temp_path(tmp_path, sizeof(tmp_path), full_path,
                           (int)getpid(), attempt) != 0) {
      continue;
    }
    fd = fs_open_temp_file(tmp_path);
    if (fd < 0 && errno != EEXIST) {
      perror("open temp file");
      return PROTOCOL_ERR_IO;
    }
  }
  if (fd < 0) {
    perror("open temp file");
    return PROTOCOL_ERR_IO;
  }

  net_recv_file_result_t recv_res = net_recv_file(conn, fd, content_size);
  if (recv_res == NET_RECV_FILE_OK) {
    if (fs_close(fd) != 0) {
      perror("close temp file");
      fs_remove_ignore_error(tmp_path);
      return PROTOCOL_ERR_IO;
    }
    if (fs_commit_temp_file(tmp_path, full_path, NULL) != 0) {
      perror("finalize temp file");
      fs_remove_ignore_error(tmp_path);
      return PROTOCOL_ERR_IO;
    }
    return PROTOCOL_OK;
  }

  if (recv_res == NET_RECV_FILE_EOF) {
    fprintf(stderr, "protocol error: unexpected EOF while receiving file\n");
    result = PROTOCOL_ERR_EOF;
  } else if (recv_res == NET_RECV_FILE_INVALID_ARGUMENT) {
    fprintf(stderr, "invalid receive file arguments\n");
    result = PROTOCOL_ERR_INVALID_ARGUMENT;
  } else {
    sock_perror("recv(file_body)");
    result = PROTOCOL_ERR_IO;
  }

  fs_close(fd);
  fs_remove_ignore_error(tmp_path);
  return result;
}

static protocol_result_t node_recv_file(
  socket_t conn,
  const char *path,
  const protocol_header_t *proto_header) {
  char *file_name = NULL;
  uint64_t content_size = 0;
  protocol_result_t result = PROTOCOL_ERR_IO;

  if (path == NULL || proto_header == NULL) {
    fprintf(stderr, "invalid file transfer handler arguments\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (proto_header->payload_size <
      (uint64_t)proto_file_transfer_prefix_size(1)) {
    fprintf(stderr, "protocol error: payload size too small\n");
    result = PROTOCOL_ERR_INVALID_ARGUMENT;
    goto REJECT;
  }

  result = proto_recv_file_transfer_prefix(conn, &file_name, &content_size);
  if (result != PROTOCOL_OK) {
    if (result == PROTOCOL_ERR_FILE_NAME_LEN) {
      fprintf(stderr, "protocol error: invalid file name length\n");
    } else if (result == PROTOCOL_ERR_ALLOC) {
      perror("malloc(file_name)");
    } else if (result == PROTOCOL_ERR_EOF) {
      fprintf(stderr, "protocol error: unexpected EOF while receiving payload\n");
    } else {
      sock_perror("protocol_recv_file_transfer_prefix");
    }
    if (result != PROTOCOL_ERR_EOF && result != PROTOCOL_ERR_IO &&
        result != PROTOCOL_ERR_ALLOC) {
      goto REJECT;
    }
    goto CLEANUP;
  }

  if (fs_validate_file_name(file_name) != 0) {
    fprintf(stderr, "invalid file name: %s\n", file_name);
    result = PROTOCOL_ERR_INVALID_FILE_NAME;
    goto REJECT;
  }

  uint64_t prefix_size = (uint64_t)proto_file_transfer_prefix_size(
    (uint16_t)strlen(file_name));
  if (proto_header->payload_size != prefix_size + content_size) {
    fprintf(stderr, "protocol error: payload size mismatch\n");
    result = PROTOCOL_ERR_PAYLOAD_SIZE_MISMATCH;
    goto REJECT;
  }

  result = node_send_response(
    conn, PROTO_PHASE_READY, PROTO_STATUS_OK, PROTOCOL_OK);
  if (result != PROTOCOL_OK) {
    sock_perror("send_res_frame(file_transfer_ready)");
    goto CLEANUP;
  }

  result = node_recv_socket_file(conn, path, file_name, content_size);
  if (result != PROTOCOL_OK) {
    if (node_send_response(
          conn, PROTO_PHASE_FINAL, PROTO_STATUS_FAILED, result) != PROTOCOL_OK) {
      sock_perror("send_res_frame(file_transfer_final_failed)");
    }
    goto CLEANUP;
  }

  result = node_send_response(
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

REJECT:
  (void)node_send_response(conn, PROTO_PHASE_READY,
                           PROTO_STATUS_REJECTED, result);

CLEANUP:
  free(file_name);
  return result;
}

static int node_recv_loop(socket_t tcp_sock, socket_t discovery_sock,
                          const char *path, uint16_t port) {
  int exit_code = 0;
  int has_discovery = is_socket_invalid(discovery_sock) ? 0 : 1;

  for (;;) {
    fd_set readfds;
    FD_ZERO(&readfds);

    FD_SET(tcp_sock, &readfds);
    if (has_discovery) {
      FD_SET(discovery_sock, &readfds);
    }

    int maxfd = (int)tcp_sock;
    if (has_discovery && discovery_sock > maxfd) maxfd = discovery_sock;

    int rc = select(maxfd + 1, &readfds, NULL, NULL, NULL);
    if (rc < 0) {
      if (errno == EINTR) continue;
      sock_perror("select");
      exit_code = 1;
      break;
    }

    if (has_discovery && FD_ISSET(discovery_sock, &readfds)) {
      discovery_handle_query(discovery_sock, port);
    }

    if (!FD_ISSET(tcp_sock, &readfds)) continue;

    socket_t conn = accept(tcp_sock, NULL, NULL);
    if (is_socket_invalid(conn)) {
      if (errno == EINTR) continue;
      sock_perror("accept");
      continue;
    }

    (void)node_handle_connection(conn, path);
    socket_close(conn);
  }

  return exit_code;
}

int node_recv(const char *path, uint16_t port) {
  int exit_code = 0;
  int discovery_ok = 0;

  if (path == NULL || strlen(path) == 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  socket_t tcp_sock;
  socket_init(&tcp_sock);

  socket_t discovery_sock;
  socket_init(&discovery_sock);

  if (create_listener_socket(port, &tcp_sock) != 0) {
    exit_code = 1;
    goto CLOSE_SOCK;
  }

  if (port < 65535u) {
    if (discovery_open(&discovery_sock, (uint16_t)(port + 1u)) == 0) {
      discovery_ok = 1;
    }
  }

  fprintf(stdout, "HFile node ready\n");
  fprintf(stdout, "  Receive Dir  %s\n", path);
  fprintf(stdout, "  Port         %u\n", (unsigned)port);
  fprintf(stdout, "  PID          %ld\n", (long)getpid());
  if (discovery_ok) {
    fprintf(stdout, "  Discovery    on (port %u)\n", (unsigned)(port + 1));
  }
  fflush(stdout);

  exit_code = node_recv_loop(tcp_sock, discovery_sock, path, port);

CLOSE_SOCK:
  socket_close(tcp_sock);
  if (discovery_ok) {
    discovery_close(discovery_sock);
  }

CLEAN_UP:
  return exit_code;
}

static const char *node_protocol_result_name(protocol_result_t res) {
  static const char *names[] = {
    "ok", "header magic", "header version", "header msg type",
    "header msg flag", "invalid argument", "file name length",
    "invalid file name", "payload size mismatch", "io", "short write",
    "alloc", "unexpected eof"
  };
  int i = (int)res;
  return (i >= 0 && (size_t)i < sizeof(names) / sizeof(names[0])) ?
    names[i] : "unknown";
}

static int node_recv_response(socket_t sock, uint8_t expected_phase,
                              const char *kind) {
  res_frame_t frame = {0};
  protocol_result_t res = recv_res_frame(sock, &frame);

  if (res != PROTOCOL_OK) {
    if (res == PROTOCOL_ERR_EOF) {
      fprintf(stderr, "node closed connection while waiting for %s response\n",
              kind);
    } else if (res == PROTOCOL_ERR_IO) {
      sock_perror("recv_res_frame");
    } else {
      fprintf(stderr, "invalid %s response frame: %s\n",
              kind,
              node_protocol_result_name(res));
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

  if (frame.status == PROTO_STATUS_OK) {
    return 0;
  }

  if ((expected_phase == PROTO_PHASE_READY &&
       frame.status == PROTO_STATUS_REJECTED) ||
      (expected_phase == PROTO_PHASE_FINAL &&
       frame.status == PROTO_STATUS_FAILED)) {
    fprintf(stderr, "node reported %s failure: %s\n", kind,
            node_protocol_result_name((protocol_result_t)frame.error_code));
    return 1;
  }

  fprintf(stderr, "invalid %s response status=%u error=%s\n", kind,
          (unsigned)frame.status,
          node_protocol_result_name((protocol_result_t)frame.error_code));
  return 1;
}

static int node_send_file_preamble(socket_t sock, const char *file_name,
                                   uint16_t file_name_len,
                                   uint64_t content_size) {
  protocol_header_t header = {0};
  uint8_t preamble_buf[HF_PROTOCOL_HEADER_SIZE + sizeof(uint16_t) +
                       HF_PROTOCOL_MAX_FILE_NAME_LEN + sizeof(uint64_t)];
  protocol_result_t proto_res = PROTOCOL_OK;

  init_header(&header);
  header.msg_type = HF_MSG_TYPE_SEND_FILE;
  header.flags = HF_MSG_FLAG_NONE;
  header.payload_size =
    (uint64_t)proto_file_transfer_prefix_size(file_name_len) + content_size;

  proto_res = encode_header(&header, preamble_buf);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode protocol header\n");
    return 1;
  }

  size_t prefix_size = proto_file_transfer_prefix_size(file_name_len);
  proto_res = encode_file_prefix(file_name, content_size,
                                 preamble_buf + HF_PROTOCOL_HEADER_SIZE);
  if (proto_res != PROTOCOL_OK) {
    fprintf(stderr, "failed to encode file_prefix\n");
    return 1;
  }

  if (proto_send_payload(sock, preamble_buf,
                         HF_PROTOCOL_HEADER_SIZE + prefix_size) != PROTOCOL_OK) {
    sock_perror("send(file_preamble)");
    return 1;
  }

  return 0;
}

int node_send(const char *file_path, const char *ip, uint16_t port) {
  int exit_code = 0;
  int in = -1;
  socket_t sock;
  socket_init(&sock);
  const char *path = file_path;
  const char *file_name = NULL;
  uint16_t file_name_len = 0;
  uint64_t content_size = 0;
  struct stat st;
  struct sockaddr_in addr;

  if (fs_basename_from_path(&path, &file_name) != 0) {
    fprintf(stderr, "invalid send path\n");
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

  if (fstat(in, &st) != 0) {
    perror("fstat");
    exit_code = 1;
    goto CLEAN_UP;
  }
  if (!S_ISREG(st.st_mode) || st.st_size < 0) {
    fprintf(stderr, "invalid source file\n");
    exit_code = 1;
    goto CLEAN_UP;
  }
  content_size = (uint64_t)st.st_size;

  if (content_size > HF_MAX_FILE_SIZE) {
    fprintf(stderr, "MAX_FILE_SIZE is 100GB\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  char discovered_ip[64];
  const char *connect_ip = ip;
  uint16_t connect_port = port;

  if (connect_ip == NULL) {
    uint16_t discovery_port = (port < 65535u) ? (uint16_t)(port + 1u) : port;
    if (discovery_find_node(discovery_port, discovered_ip,
                            sizeof(discovered_ip), &connect_port) != 0) {
      fprintf(stderr, "no HFile node found on LAN\n");
      exit_code = 1;
      goto CLEAN_UP;
    }
    connect_ip = discovered_ip;
    fprintf(stdout, "discovered node at %s:%u\n", connect_ip,
            (unsigned)connect_port);
  }

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (is_socket_invalid(sock)) {
    sock_perror("socket");
    exit_code = 1;
    goto CLEAN_UP;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(connect_port);
  if (inet_pton(AF_INET, connect_ip, &addr.sin_addr) != 1) {
    perror("inet_pton");
    exit_code = 1;
    goto CLEAN_UP;
  }
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    sock_perror("connect");
    exit_code = 1;
    goto CLEAN_UP;
  }
  if (node_send_file_preamble(sock, file_name, file_name_len, content_size) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (node_recv_response(sock, PROTO_PHASE_READY, "transfer") != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  net_send_file_result_t send_file_res =
    net_send_file(sock, in, content_size);
  if (send_file_res != NET_SEND_FILE_OK) {
    if (send_file_res == NET_SEND_FILE_SOURCE_CHANGED) {
      fprintf(stderr, "source file changed during transfer\n");
    } else if (send_file_res == NET_SEND_FILE_INVALID_ARGUMENT) {
      fprintf(stderr, "invalid raw transfer arguments\n");
    } else {
      sock_perror("sendfile");
    }
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (node_recv_response(sock, PROTO_PHASE_FINAL, "transfer") != 0) {
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
