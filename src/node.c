#include "discovery.h"
#include "fs.h"
#include "net.h"
#include "node.h"
#include "protocol.h"
#include "shutdown.h"
#include "transfer_io.h"

#include <stddef.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

#define SERVER_CONTROL_RECV_TIMEOUT_MS 15000u
#define NODE_CONTROL_SOCKET_TIMEOUT_MS 30000u

typedef struct {
  char *file_name;
  uint64_t content_size;
} node_file_request_t;

static protocol_result_t node_recv_file(
  socket_t conn,
  const char *path,
  const protocol_header_t *proto_header);
static protocol_result_t node_send_response(socket_t conn,
                                            uint8_t phase,
                                            uint8_t status,
                                            protocol_result_t error_code);
static void node_file_request_cleanup(node_file_request_t *request);

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

static int node_handle_single_connection(socket_t conn, const char *path) {
  if (net_set_recv_timeout(conn, SERVER_CONTROL_RECV_TIMEOUT_MS) != 0) {
    sock_perror("setsockopt(SO_RCVTIMEO)");
  }

  return node_handle_connection(conn, path);
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

static void node_file_request_cleanup(node_file_request_t *request) {
  if (request == NULL) {
    return;
  }
  free(request->file_name);
  request->file_name = NULL;
  request->content_size = 0;
}

static protocol_result_t node_read_file_request(
  socket_t conn,
  const protocol_header_t *proto_header,
  node_file_request_t *request) {
  uint64_t prefix_size = 0;
  protocol_result_t result = PROTOCOL_ERR_IO;

  if (proto_header == NULL || request == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  request->file_name = NULL;
  request->content_size = 0;

  if (proto_header->payload_size <
      (uint64_t)proto_file_transfer_prefix_size(1)) {
    fprintf(stderr, "protocol error: payload size too small\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  result = proto_recv_file_transfer_prefix(
    conn, &request->file_name, &request->content_size);
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
    return result;
  }

  if (fs_validate_file_name(request->file_name) != 0) {
    fprintf(stderr, "invalid file name: %s\n", request->file_name);
    return PROTOCOL_ERR_INVALID_FILE_NAME;
  }

  prefix_size = (uint64_t)proto_file_transfer_prefix_size(
    (uint16_t)strlen(request->file_name));
  if (proto_header->payload_size != prefix_size + request->content_size) {
    fprintf(stderr, "protocol error: payload size mismatch\n");
    return PROTOCOL_ERR_PAYLOAD_SIZE_MISMATCH;
  }

  return PROTOCOL_OK;
}

static protocol_result_t node_recv_file(
  socket_t conn,
  const char *path,
  const protocol_header_t *proto_header) {
  node_file_request_t request = {0};
  char saved_path[4096];
  protocol_result_t result = PROTOCOL_ERR_IO;

  if (path == NULL) {
    fprintf(stderr, "invalid file transfer handler arguments\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  result = node_read_file_request(conn, proto_header, &request);
  if (result != PROTOCOL_OK) {
    if (result != PROTOCOL_ERR_EOF && result != PROTOCOL_ERR_IO &&
        result != PROTOCOL_ERR_ALLOC) {
      (void)node_send_response(
        conn, PROTO_PHASE_READY, PROTO_STATUS_REJECTED, result);
    }
    goto CLEANUP;
  }

  result = node_send_response(
    conn, PROTO_PHASE_READY, PROTO_STATUS_OK, PROTOCOL_OK);
  if (result != PROTOCOL_OK) {
    sock_perror("send_res_frame(file_transfer_ready)");
    goto CLEANUP;
  }

  if (net_set_recv_timeout(
        conn, net_transfer_timeout_ms(request.content_size)) != 0) {
    sock_perror("setsockopt(SO_RCVTIMEO)");
    result = PROTOCOL_ERR_IO;
    goto CLEANUP;
  }

  result = transfer_recv_socket_file(conn, path, request.file_name,
                                     request.content_size,
                                     "recv(file_body)",
                                     "protocol error: unexpected EOF while receiving file",
                                     saved_path, sizeof(saved_path));
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
          request.file_name, (unsigned long long)request.content_size);
  fflush(stdout);

  result = PROTOCOL_OK;
  goto CLEANUP;

CLEANUP:
  node_file_request_cleanup(&request);
  return result;
}

static int node_recv_once(socket_t tcp_sock, socket_t discovery_sock,
                          const char *path, uint16_t port) {
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
        discovery_handle_query(discovery_sock, port);
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

    exit_code = node_handle_single_connection(conn, path);
    socket_close(conn);
    break;
  }

  return exit_code;
}

static long node_current_pid_long(void) {
  return (long)getpid();
}

static void node_print_access_details(const char *path, uint16_t port,
                                      long pid, int discovery_ok) {
  if (path == NULL) {
    return;
  }

  fprintf(stdout, "HFile node ready\n");
  fprintf(stdout, "  Receive Dir  %s\n", path);
  fprintf(stdout, "  Port         %u\n", (unsigned)port);
  fprintf(stdout, "  PID          %ld\n", pid);
  if (discovery_ok) {
    fprintf(stdout, "  Discovery    on (port %u)\n",
            (unsigned)(port + 1));
  }
  fflush(stdout);
}

static void node_print_shutdown_notice(void) {
  int add_leading_newline = 0;

  if (shutdown_signal_number() == SIGINT) {
    add_leading_newline = isatty(fileno(stderr)) ? 1 : 0;
  }

  if (add_leading_newline) {
    fprintf(stderr, "\nshutdown requested, stopping node\n");
  } else {
    fprintf(stderr, "shutdown requested, stopping node\n");
  }
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

  node_print_access_details(path, port, node_current_pid_long(), discovery_ok);

  exit_code = node_recv_once(tcp_sock, discovery_sock, path, port);
  if (shutdown_requested()) {
    node_print_shutdown_notice();
  }

CLOSE_SOCK:
  socket_close(tcp_sock);
  if (discovery_ok) {
    discovery_close(discovery_sock);
  }

CLEAN_UP:
  return exit_code;
}

static const char *node_protocol_result_name(protocol_result_t res) {
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

static int node_recv_response_frame(socket_t sock,
                                    uint8_t expected_phase,
                                    const char *kind,
                                    res_frame_t *frame_out) {
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

  if (frame_out != NULL) {
    *frame_out = frame;
  }
  return 0;
}

static int node_check_response(const res_frame_t *frame,
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
      fprintf(stderr, "node reported %s failure: %s\n",
              kind,
              node_protocol_result_name((protocol_result_t)frame->error_code));
      return 1;
    }

    fprintf(stderr, "invalid %s ready response status=%u error=%s\n",
            kind,
            (unsigned)frame->status,
            node_protocol_result_name((protocol_result_t)frame->error_code));
    return 1;
  }

  if (expected_phase == PROTO_PHASE_FINAL) {
    if (frame->status == PROTO_STATUS_FAILED) {
      fprintf(stderr, "node reported %s failure: %s\n",
              kind,
              node_protocol_result_name((protocol_result_t)frame->error_code));
      return 1;
    }

    fprintf(stderr, "invalid %s final response status=%u error=%s\n",
            kind,
            (unsigned)frame->status,
            node_protocol_result_name((protocol_result_t)frame->error_code));
    return 1;
  }

  fprintf(stderr, "invalid expected %s phase=%u\n", kind,
          (unsigned)expected_phase);
  return 1;
}

static int node_recv_checked_response(socket_t sock,
                                      uint8_t expected_phase,
                                      const char *kind,
                                      res_frame_t *frame_out) {
  res_frame_t frame = {0};

  if (node_recv_response_frame(sock, expected_phase, kind, &frame) != 0) {
    return 1;
  }
  if (node_check_response(&frame, expected_phase, kind) != 0) {
    return 1;
  }
  if (frame_out != NULL) {
    *frame_out = frame;
  }
  return 0;
}

static int node_send_header_payload(socket_t sock,
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

static int node_connect(const char *ip, uint16_t port, socket_t *sock_out) {
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

  if (net_set_socket_timeouts(sock, NODE_CONTROL_SOCKET_TIMEOUT_MS) != 0) {
    sock_perror("setsockopt(node_timeout)");
    socket_close(sock);
    return 1;
  }

  *sock_out = sock;
  return 0;
}

static int node_get_file_size(int in, uint64_t *content_size_out) {
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

static int node_send_file_body(int in, socket_t sock, uint64_t content_size) {
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

int node_send(const char *file_path, const char *ip, uint16_t port) {
  int exit_code = 0;
  int in = -1;
  socket_t sock;
  socket_init(&sock);
  const char *path = file_path;
  const char *file_name = NULL;
  uint16_t file_name_len = 0;
  uint64_t content_size = 0;

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

  if (proto_file_transfer_prefix_size(file_name_len) > CHUNK_SIZE) {
    fprintf(stderr, "protocol payload prefix exceeds buffer size\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (node_get_file_size(in, &content_size) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

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

  if (node_connect(connect_ip, connect_port, &sock) != 0) {
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
  if (node_send_header_payload(sock, HF_MSG_TYPE_SEND_FILE, payload_size,
                               file_prefix_buf, file_prefix_size,
                               "send(file_preamble)") != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  res_frame_t r_f = {0};
  if (node_recv_checked_response(sock, PROTO_PHASE_READY, "transfer", &r_f) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (net_set_socket_timeouts(sock, net_transfer_timeout_ms(content_size)) != 0) {
    sock_perror("setsockopt(node_transfer_timeout)");
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (node_send_file_body(in, sock, content_size) != 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (shutdown(sock, SHUT_WR) < 0) {
    sock_perror("shutdown(SHUT_WR)");
  }
  if (node_recv_checked_response(sock, PROTO_PHASE_FINAL, "transfer", &r_f) != 0) {
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
