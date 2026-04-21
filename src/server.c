#include "app_service.h"
#include "cli.h"
#include "control.h"
#include "daemon_state.h"
#include "http.h"
#include "message_store.h"
#include "net.h"
#include "protocol.h"
#include "shutdown.h"
#include "server.h"

#include <stddef.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"

#ifdef _WIN32
  #include <io.h>
  #include <process.h>
#else
  #include <pthread.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

typedef struct {
  socket_t conn;
  server_opt_t opt;
  struct server_conn_entry_t *entry;
} server_conn_ctx_t;

typedef struct server_conn_entry_t {
  socket_t conn;
  int closing;
  struct server_conn_entry_t *next;
} server_conn_entry_t;

typedef enum {
  SERVER_CONN_KIND_INVALID = 0,
  SERVER_CONN_KIND_HTTP,
  SERVER_CONN_KIND_PROTOCOL,
} server_conn_kind_t;

typedef struct {
#ifdef _WIN32
  HANDLE handle;
#else
  pthread_t tid;
#endif
} server_thread_t;

typedef struct {
  int initialized;
  int shutting_down;
  unsigned int active_count;
  server_conn_entry_t *head;
#ifdef _WIN32
  CRITICAL_SECTION mutex;
  CONDITION_VARIABLE cond;
#else
  pthread_mutex_t mutex;
  pthread_cond_t cond;
#endif
} server_conn_tracker_t;

typedef struct {
  char receive_dir[4096];
  char log_path[4096];
  uint16_t port;
  int daemon_mode;
  char last_web_url[256];
} server_state_watcher_ctx_t;

static server_conn_tracker_t g_server_conn_tracker = {0};

#define SERVER_STATE_REFRESH_INTERVAL_SECONDS 60u

static int server_handle_text_message(socket_t conn,
                                      const protocol_header_t *proto_header);
static protocol_result_t server_handle_file_transfer(
  socket_t conn,
  const server_opt_t *ser_opt,
  const protocol_header_t *proto_header);
static protocol_result_t server_handle_get_file(
  socket_t conn,
  const server_opt_t *ser_opt,
  const protocol_header_t *proto_header);
static protocol_result_t server_send_response(socket_t conn,
                                              uint8_t phase,
                                              uint8_t status,
                                              protocol_result_t error_code);
static int server_build_current_web_url(uint16_t port,
                                        char *web_url_out,
                                        size_t web_url_out_cap);
static int server_persist_daemon_state(const char *receive_dir,
                                       uint16_t port,
                                       const char *log_path,
                                       int daemon_mode,
                                       char *web_url_out,
                                       size_t web_url_out_cap);
static void server_sleep_ms(uint32_t timeout_ms);
static int server_wait_for_refresh_interval(uint32_t seconds);
static int server_start_state_watcher(server_thread_t *thread_out,
                                      server_state_watcher_ctx_t **ctx_out,
                                      const char *receive_dir,
                                      uint16_t port,
                                      const char *log_path,
                                      int daemon_mode,
                                      const char *initial_web_url);
static void server_join_state_watcher(server_thread_t *thread,
                                      server_state_watcher_ctx_t *ctx);

static int server_conn_tracker_init(void) {
  if (g_server_conn_tracker.initialized) {
    return 0;
  }

#ifdef _WIN32
  InitializeCriticalSection(&g_server_conn_tracker.mutex);
  InitializeConditionVariable(&g_server_conn_tracker.cond);
#else
  if (pthread_mutex_init(&g_server_conn_tracker.mutex, NULL) != 0) {
    return 1;
  }
  if (pthread_cond_init(&g_server_conn_tracker.cond, NULL) != 0) {
    (void)pthread_mutex_destroy(&g_server_conn_tracker.mutex);
    return 1;
  }
#endif

  g_server_conn_tracker.initialized = 1;
  g_server_conn_tracker.shutting_down = 0;
  g_server_conn_tracker.active_count = 0;
  g_server_conn_tracker.head = NULL;
  return 0;
}

static void server_conn_tracker_abort_entry(server_conn_entry_t *entry) {
  if (entry == NULL || entry->closing) {
    return;
  }
  entry->closing = 1;
#ifdef _WIN32
  (void)shutdown(entry->conn, SD_BOTH);
#else
  (void)shutdown(entry->conn, SHUT_RDWR);
#endif
}

static server_conn_entry_t *server_conn_tracker_begin(socket_t conn) {
  server_conn_entry_t *entry = (server_conn_entry_t *)malloc(sizeof(*entry));
  if (entry == NULL) {
    return NULL;
  }

  entry->conn = conn;
  entry->closing = 0;
  entry->next = NULL;

#ifdef _WIN32
  EnterCriticalSection(&g_server_conn_tracker.mutex);
#else
  (void)pthread_mutex_lock(&g_server_conn_tracker.mutex);
#endif
  if (g_server_conn_tracker.shutting_down) {
#ifdef _WIN32
    LeaveCriticalSection(&g_server_conn_tracker.mutex);
#else
    (void)pthread_mutex_unlock(&g_server_conn_tracker.mutex);
#endif
    free(entry);
    return NULL;
  }

  entry->next = g_server_conn_tracker.head;
  g_server_conn_tracker.head = entry;
  g_server_conn_tracker.active_count++;
#ifdef _WIN32
  LeaveCriticalSection(&g_server_conn_tracker.mutex);
#else
  (void)pthread_mutex_unlock(&g_server_conn_tracker.mutex);
#endif
  return entry;
}

static void server_conn_tracker_end(server_conn_entry_t *entry) {
#ifdef _WIN32
  EnterCriticalSection(&g_server_conn_tracker.mutex);
#else
  (void)pthread_mutex_lock(&g_server_conn_tracker.mutex);
#endif

  server_conn_entry_t **link = &g_server_conn_tracker.head;
  while (*link != NULL) {
    if (*link == entry) {
      *link = entry->next;
      break;
    }
    link = &(*link)->next;
  }
  if (g_server_conn_tracker.active_count > 0) {
    g_server_conn_tracker.active_count--;
  }
  if (g_server_conn_tracker.shutting_down && g_server_conn_tracker.active_count == 0) {
#ifdef _WIN32
    WakeAllConditionVariable(&g_server_conn_tracker.cond);
#else
    (void)pthread_cond_broadcast(&g_server_conn_tracker.cond);
#endif
  }

#ifdef _WIN32
  LeaveCriticalSection(&g_server_conn_tracker.mutex);
#else
  (void)pthread_mutex_unlock(&g_server_conn_tracker.mutex);
#endif

  if (entry != NULL) {
    free(entry);
  }
}

static void server_conn_tracker_shutdown_all(void) {
  if (!g_server_conn_tracker.initialized) {
    return;
  }

#ifdef _WIN32
  EnterCriticalSection(&g_server_conn_tracker.mutex);
#else
  (void)pthread_mutex_lock(&g_server_conn_tracker.mutex);
#endif
  g_server_conn_tracker.shutting_down = 1;
  for (server_conn_entry_t *entry = g_server_conn_tracker.head;
       entry != NULL;
       entry = entry->next) {
    server_conn_tracker_abort_entry(entry);
  }
  if (g_server_conn_tracker.active_count == 0) {
#ifdef _WIN32
    WakeAllConditionVariable(&g_server_conn_tracker.cond);
#else
    (void)pthread_cond_broadcast(&g_server_conn_tracker.cond);
#endif
  }
#ifdef _WIN32
  LeaveCriticalSection(&g_server_conn_tracker.mutex);
#else
  (void)pthread_mutex_unlock(&g_server_conn_tracker.mutex);
#endif
}

static void server_conn_tracker_wait_idle(void) {
  if (!g_server_conn_tracker.initialized) {
    return;
  }

#ifdef _WIN32
  EnterCriticalSection(&g_server_conn_tracker.mutex);
  while (g_server_conn_tracker.active_count > 0) {
    SleepConditionVariableCS(&g_server_conn_tracker.cond,
                             &g_server_conn_tracker.mutex, INFINITE);
  }
  LeaveCriticalSection(&g_server_conn_tracker.mutex);
#else
  (void)pthread_mutex_lock(&g_server_conn_tracker.mutex);
  while (g_server_conn_tracker.active_count > 0) {
    (void)pthread_cond_wait(&g_server_conn_tracker.cond,
                            &g_server_conn_tracker.mutex);
  }
  (void)pthread_mutex_unlock(&g_server_conn_tracker.mutex);
#endif
}

static void server_conn_tracker_cleanup(void) {
  if (!g_server_conn_tracker.initialized) {
    return;
  }

#ifdef _WIN32
  DeleteCriticalSection(&g_server_conn_tracker.mutex);
#else
  (void)pthread_cond_destroy(&g_server_conn_tracker.cond);
  (void)pthread_mutex_destroy(&g_server_conn_tracker.mutex);
#endif

  g_server_conn_tracker.initialized = 0;
  g_server_conn_tracker.shutting_down = 0;
  g_server_conn_tracker.active_count = 0;
  g_server_conn_tracker.head = NULL;
}

static int server_set_connection_recv_timeout(socket_t conn, uint32_t timeout_ms) {
#ifdef _WIN32
  DWORD timeout = (DWORD)timeout_ms;
  return setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO,
                    (const char *)&timeout, sizeof(timeout)) == SOCKET_ERROR ? 1 : 0;
#else
  struct timeval tv;
  tv.tv_sec = (time_t)(timeout_ms / 1000u);
  tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
  return setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ? 1 : 0;
#endif
}

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

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (is_socket_invalid(sock)) {
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

static server_conn_kind_t server_detect_connection_kind(socket_t conn) {
  uint8_t buf[8];
  ssize_t n = 0;

#ifdef _WIN32
  n = recv(conn, (char *)buf, (int)sizeof(buf), MSG_PEEK);
  if (n == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err == WSAEINTR || err == WSAEWOULDBLOCK) {
      return SERVER_CONN_KIND_INVALID;
    }
    return SERVER_CONN_KIND_INVALID;
  }
#else
  n = recv(conn, buf, sizeof(buf), MSG_PEEK);
  if (n < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      return SERVER_CONN_KIND_INVALID;
    }
    return SERVER_CONN_KIND_INVALID;
  }
#endif
  if (n < 2) {
    return SERVER_CONN_KIND_INVALID;
  }

  if (buf[0] == (uint8_t)(HF_PROTOCOL_MAGIC >> 8) &&
      buf[1] == (uint8_t)(HF_PROTOCOL_MAGIC & 0xFFu)) {
    return SERVER_CONN_KIND_PROTOCOL;
  }

  if (buf[0] >= 'A' && buf[0] <= 'Z') {
    return SERVER_CONN_KIND_HTTP;
  }

  if (n >= 4) {
    if (memcmp(buf, "GET ", 4) == 0 || memcmp(buf, "PUT ", 4) == 0) {
      return SERVER_CONN_KIND_HTTP;
    }
  }
  if (n >= 5) {
    if (memcmp(buf, "POST ", 5) == 0 || memcmp(buf, "HEAD ", 5) == 0) {
      return SERVER_CONN_KIND_HTTP;
    }
  }
  if (n >= 7) {
    if (memcmp(buf, "DELETE ", 7) == 0 || memcmp(buf, "OPTIONS", 7) == 0) {
      return SERVER_CONN_KIND_HTTP;
    }
  }

  return SERVER_CONN_KIND_PROTOCOL;
}

static int handle_protocol_connection(socket_t conn,
                                             const server_opt_t *ser_opt) {
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
    case HF_MSG_TYPE_TEXT_MESSAGE:
      return server_handle_text_message(conn, &proto_header);

    case HF_MSG_TYPE_SEND_FILE:
      return server_handle_file_transfer(conn, ser_opt, &proto_header) == PROTOCOL_OK ? 0 : 1;

    case HF_MSG_TYPE_GET_FILE:
      return server_handle_get_file(conn, ser_opt, &proto_header) == PROTOCOL_OK ? 0 : 1;

    default:
      fprintf(stderr, "protocol error: unsupported message type: %u\n",
              (unsigned)proto_header.msg_type);
      return 1;
  }
}

#ifdef _WIN32
static unsigned __stdcall server_connection_thread_main(void *arg) {
#else
static void *server_connection_thread_main(void *arg) {
#endif
  server_conn_ctx_t *ctx = (server_conn_ctx_t *)arg;
  socket_t conn = ctx->conn;
  server_opt_t opt = ctx->opt;
  server_conn_entry_t *entry = ctx->entry;

  free(ctx);

  switch (server_detect_connection_kind(conn)) {
    case SERVER_CONN_KIND_HTTP:
      (void)handle_http_connection(conn, &opt);
      break;
    case SERVER_CONN_KIND_PROTOCOL:
      (void)handle_protocol_connection(conn, &opt);
      break;
    default:
      fprintf(stderr, "we don't support this mode\n");
      break;
  }

  socket_close(conn);
  server_conn_tracker_end(entry);

#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

static int server_start_connection_thread(socket_t conn,
                                          const server_opt_t *ser_opt) {
  server_conn_ctx_t *ctx = (server_conn_ctx_t *)malloc(sizeof(*ctx));
  server_conn_entry_t *entry = NULL;
  if (ctx == NULL) {
    perror("malloc(server_conn_ctx)");
    return 1;
  }

  if (server_set_connection_recv_timeout(conn, 15000u) != 0) {
    sock_perror("setsockopt(SO_RCVTIMEO)");
  }

  entry = server_conn_tracker_begin(conn);
  if (entry == NULL) {
    free(ctx);
    return 1;
  }

  ctx->conn = conn;
  ctx->opt = *ser_opt;
  ctx->entry = entry;

#ifdef _WIN32
  uintptr_t handle = _beginthreadex(NULL, 0, server_connection_thread_main, ctx, 0, NULL);
  if (handle == 0) {
    fprintf(stderr, "_beginthreadex(server_conn) failed\n");
    server_conn_tracker_end(entry);
    free(ctx);
    return 1;
  }
  CloseHandle((HANDLE)handle);
#else
  server_thread_t thread = {0};
  int err = pthread_create(&thread.tid, NULL, server_connection_thread_main, ctx);
  if (err != 0) {
    fprintf(stderr, "pthread_create(server_conn): %s\n", strerror(err));
    server_conn_tracker_end(entry);
    free(ctx);
    return 1;
  }
  (void)pthread_detach(thread.tid);
#endif

  return 0;
}

static int server_handle_text_message(socket_t conn,
                                      const protocol_header_t *proto_header) {
  char *message = NULL;
  protocol_result_t result = PROTOCOL_ERR_INVALID_ARGUMENT;
  uint8_t status = PROTO_STATUS_FAILED;

  if (proto_header == NULL) {
    fprintf(stderr, "invalid text message handler arguments\n");
    return 1;
  }

  if (proto_header->flags != HF_MSG_FLAG_NONE) {
    fprintf(stderr, "protocol error: text message has unsupported flags\n");
    result = PROTOCOL_ERR_HEADER_MSG_FLAG;
    goto SEND_RESPONSE;
  }

  if (proto_header->payload_size > HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE) {
    fprintf(stderr, "protocol error: text message too large\n");
    result = PROTOCOL_ERR_MSG_TOO_LARGE;
    goto SEND_RESPONSE;
  }

  size_t message_len = (size_t)proto_header->payload_size;
  message = (char *)malloc(message_len + 1u);
  if (message == NULL) {
    perror("malloc(message)");
    result = PROTOCOL_ERR_ALLOC;
    goto SEND_RESPONSE;
  }

  if (message_len > 0) {
    ssize_t n = recv_all(conn, message, message_len);
    if (n != (ssize_t)message_len) {
      if (n < 0) {
        sock_perror("recv_all(message)");
        result = PROTOCOL_ERR_IO;
      } else {
        fprintf(stderr,
                "protocol error: unexpected EOF while receiving message\n");
        result = PROTOCOL_ERR_EOF;
      }
      goto SEND_RESPONSE;
    }
  }

  message[message_len] = '\0';
  result = app_submit_message(message);
  if (result != PROTOCOL_OK) {
    goto SEND_RESPONSE;
  }

  status = PROTO_STATUS_OK;
  result = PROTOCOL_OK;

SEND_RESPONSE:
  if (server_send_response(conn, PROTO_PHASE_FINAL, status, result) != PROTOCOL_OK) {
    sock_perror("send_res_frame(text_message_final)");
  }
  if (message != NULL) free(message);
  return result == PROTOCOL_OK ? 0 : 1;
}

static protocol_result_t server_send_response(
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

static protocol_result_t server_handle_file_transfer(
  socket_t conn,
  const server_opt_t *ser_opt,
  const protocol_header_t *proto_header) {
  char *file_name = NULL;
  char saved_path[4096];
  uint64_t content_size = 0;
  uint64_t prefix_size = 0;
  protocol_result_t result = PROTOCOL_ERR_IO;

  if (ser_opt == NULL) {
    fprintf(stderr, "invalid file transfer handler arguments\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (proto_header->payload_size <
      (uint64_t)proto_file_transfer_prefix_size(1)) {
    fprintf(stderr, "protocol error: payload size too small\n");
    (void)server_send_response(
      conn, PROTO_PHASE_READY, PROTO_STATUS_REJECTED, PROTOCOL_ERR_INVALID_ARGUMENT);
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  result = proto_recv_file_transfer_prefix(conn, &file_name, &content_size);
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
    goto CLEANUP;
  }

  if (fs_validate_file_name(file_name) != 0) {
    fprintf(stderr, "invalid file name: %s\n", file_name);
    result = PROTOCOL_ERR_INVALID_FILE_NAME;
    goto SEND_READY_REJECT;
  }

  prefix_size = (uint64_t)proto_file_transfer_prefix_size((uint16_t)strlen(file_name));
  if (proto_header->payload_size != prefix_size + content_size) {
    fprintf(stderr, "protocol error: payload size mismatch\n");
    result = PROTOCOL_ERR_PAYLOAD_SIZE_MISMATCH;
    goto SEND_READY_REJECT;
  }

  result = server_send_response(
    conn, PROTO_PHASE_READY, PROTO_STATUS_OK, PROTOCOL_OK);
  if (result != PROTOCOL_OK) {
    sock_perror("send_res_frame(file_transfer_ready)");
    goto CLEANUP;
  }

  result = app_receive_file(conn, ser_opt->path, file_name, content_size,
                            APP_UPLOAD_PROTOCOL, saved_path,
                            sizeof(saved_path));
  if (result != PROTOCOL_OK) {
    if (server_send_response(
          conn, PROTO_PHASE_FINAL, PROTO_STATUS_FAILED, result) != PROTOCOL_OK) {
      sock_perror("send_res_frame(file_transfer_final_failed)");
    }
    goto CLEANUP;
  }

  result = server_send_response(
    conn, PROTO_PHASE_FINAL, PROTO_STATUS_OK, PROTOCOL_OK);
  if (result != PROTOCOL_OK) {
    sock_perror("send_res_frame(file_transfer_final_ok)");
    goto CLEANUP;
  }

  result = PROTOCOL_OK;
  goto CLEANUP;

SEND_READY_REJECT:
  if (server_send_response(
        conn, PROTO_PHASE_READY, PROTO_STATUS_REJECTED, result) != PROTOCOL_OK) {
    sock_perror("send_res_frame(file_transfer_ready_rejected)");
  }

CLEANUP:
  if (file_name != NULL) free(file_name);
  return result;
}

static protocol_result_t server_handle_get_file(
  socket_t conn,
  const server_opt_t *ser_opt,
  const protocol_header_t *proto_header) {
  char *file_name = NULL;
  app_download_t download = {.fd = -1};
  protocol_result_t result = PROTOCOL_ERR_IO;

  if (ser_opt == NULL) {
    fprintf(stderr, "invalid get handler arguments\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (proto_header->payload_size < (uint64_t)proto_file_name_only_size(1)) {
    fprintf(stderr, "protocol error: get payload size too small\n");
    (void)server_send_response(
      conn, PROTO_PHASE_READY, PROTO_STATUS_REJECTED, PROTOCOL_ERR_INVALID_ARGUMENT);
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  result = proto_recv_file_name_only(conn, &file_name);
  if (result != PROTOCOL_OK) {
    if (result == PROTOCOL_ERR_FILE_NAME_LEN) {
      fprintf(stderr, "protocol error: invalid get file name length\n");
    } else if (result == PROTOCOL_ERR_ALLOC) {
      perror("malloc(file_name)");
    } else if (result == PROTOCOL_ERR_EOF) {
      fprintf(stderr, "protocol error: unexpected EOF while receiving get request\n");
    } else {
      sock_perror("proto_recv_file_name_only");
    }
    goto SEND_READY_REJECT;
  }

  if (fs_validate_file_name(file_name) != 0) {
    fprintf(stderr, "invalid get file name: %s\n", file_name);
    result = PROTOCOL_ERR_INVALID_FILE_NAME;
    goto SEND_READY_REJECT;
  }

  if (proto_header->payload_size !=
      (uint64_t)proto_file_name_only_size((uint16_t)strlen(file_name))) {
    fprintf(stderr, "protocol error: get payload size mismatch\n");
    result = PROTOCOL_ERR_PAYLOAD_SIZE_MISMATCH;
    goto SEND_READY_REJECT;
  }

  result = app_prepare_download(ser_opt->path, file_name, &download);
  if (result != PROTOCOL_OK) {
    if (result == PROTOCOL_ERR_IO) {
      result = PROTOCOL_ERR_INVALID_ARGUMENT;
    }
    goto SEND_READY_REJECT;
  }

  {
    uint8_t ready_prefix_buf[HF_PROTOCOL_RES_FRAME_SIZE + sizeof(uint16_t) +
                             HF_PROTOCOL_MAX_FILE_NAME_LEN + sizeof(uint64_t)];
    res_frame_t ready_frame = {0};
    uint8_t prefix_buf[sizeof(uint16_t) + HF_PROTOCOL_MAX_FILE_NAME_LEN + sizeof(uint64_t)];
    size_t prefix_size = proto_file_transfer_prefix_size((uint16_t)strlen(file_name));

    ready_frame.phase = PROTO_PHASE_READY;
    ready_frame.status = PROTO_STATUS_OK;
    ready_frame.error_code = PROTOCOL_OK;
    result = encode_res_frame(&ready_frame, ready_prefix_buf);
    if (result != PROTOCOL_OK) {
      goto CLEANUP;
    }

    result = encode_file_prefix(file_name, download.info.size, prefix_buf);
    if (result != PROTOCOL_OK) {
      goto SEND_FINAL_FAILED;
    }

    memcpy(ready_prefix_buf + HF_PROTOCOL_RES_FRAME_SIZE, prefix_buf, prefix_size);
    result = proto_send_payload(
      conn,
      ready_prefix_buf,
      HF_PROTOCOL_RES_FRAME_SIZE + prefix_size);
    if (result != PROTOCOL_OK) {
      sock_perror("send(get_ready_prefix)");
      goto CLEANUP;
    }
  }

  {
    net_send_file_result_t send_res = net_send_file_best_effort(conn, download.fd,
                                                                download.info.size);
    if (send_res != NET_SEND_FILE_OK) {
      result = PROTOCOL_ERR_IO;
      goto SEND_FINAL_FAILED;
    }
  }

  result = server_send_response(
    conn, PROTO_PHASE_FINAL, PROTO_STATUS_OK, PROTOCOL_OK);
  if (result != PROTOCOL_OK) {
    sock_perror("send_res_frame(get_final_ok)");
    goto CLEANUP;
  }

  result = PROTOCOL_OK;
  goto CLEANUP;

SEND_FINAL_FAILED:
  if (server_send_response(
        conn, PROTO_PHASE_FINAL, PROTO_STATUS_FAILED, result) != PROTOCOL_OK) {
    sock_perror("send_res_frame(get_final_failed)");
  }
  goto CLEANUP;

SEND_READY_REJECT:
  if (server_send_response(
        conn, PROTO_PHASE_READY, PROTO_STATUS_REJECTED, result) != PROTOCOL_OK) {
    sock_perror("send_res_frame(get_ready_rejected)");
  }

CLEANUP:
  app_download_cleanup(&download);
  if (file_name != NULL) {
    free(file_name);
  }
  return result;
}

static int server_run_listener(socket_t sock, const server_opt_t *ser_opt) {
  int exit_code = 0;

  for (;;) {
    int ready = 0;
    if (shutdown_requested()) {
      exit_code = shutdown_exit_code();
      break;
    }

    if (net_wait_readable(sock, 250u, &ready) != 0) {
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
      sock_perror("select(accept)");
      exit_code = 1;
      continue;
    }
    if (!ready) {
      continue;
    }

    socket_t conn = accept(sock, NULL, NULL);
#ifdef _WIN32
    if (is_socket_invalid(conn)) {
      if (WSAGetLastError() == WSAEINTR) continue;
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
#else
    if (is_socket_invalid(conn)) {
      if (errno == EINTR) continue;
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
#endif
      sock_perror("accept");
      continue;
    }

    if (server_start_connection_thread(conn, ser_opt) != 0) {
      socket_close(conn);
      exit_code = 1;
    }
  }

  return exit_code;
}

static long server_current_pid_long(void) {
#ifdef _WIN32
  return (long)_getpid();
#else
  return (long)getpid();
#endif
}

static int server_build_current_web_url(uint16_t port,
                                        char *web_url_out,
                                        size_t web_url_out_cap) {
  int phone_reachable = 0;

  if (web_url_out == NULL || web_url_out_cap == 0 || port == 0) {
    return 1;
  }

  if (control_build_url(port, web_url_out, web_url_out_cap, &phone_reachable) != 0) {
    web_url_out[0] = '\0';
    return 1;
  }

  return 0;
}

static int server_persist_daemon_state(const char *receive_dir,
                                       uint16_t port,
                                       const char *log_path,
                                       int daemon_mode,
                                       char *web_url_out,
                                       size_t web_url_out_cap) {
  daemon_state_t state = {0};

  if (receive_dir == NULL || receive_dir[0] == '\0' || port == 0) {
    return 1;
  }

  state.pid = server_current_pid_long();
  (void)snprintf(state.receive_dir, sizeof(state.receive_dir), "%s", receive_dir);
  (void)snprintf(state.log_path, sizeof(state.log_path), "%s",
                 log_path != NULL ? log_path : "");
  state.port = port;
  state.daemon_mode = daemon_mode ? 1 : 0;

  if (server_build_current_web_url(port, state.web_url, sizeof(state.web_url)) != 0) {
    state.web_url[0] = '\0';
  }

  if (web_url_out != NULL && web_url_out_cap > 0) {
    (void)snprintf(web_url_out, web_url_out_cap, "%s", state.web_url);
  }

  return daemon_state_write(&state);
}

static void server_sleep_ms(uint32_t timeout_ms) {
#ifdef _WIN32
  Sleep((DWORD)timeout_ms);
#else
  usleep((useconds_t)timeout_ms * 1000u);
#endif
}

static int server_wait_for_refresh_interval(uint32_t seconds) {
  uint32_t remaining_ms = seconds * 1000u;

  while (remaining_ms > 0) {
    uint32_t chunk_ms = remaining_ms > 1000u ? 1000u : remaining_ms;
    if (shutdown_requested()) {
      return 1;
    }
    server_sleep_ms(chunk_ms);
    remaining_ms -= chunk_ms;
  }

  return shutdown_requested() ? 1 : 0;
}

#ifdef _WIN32
static unsigned __stdcall server_state_watcher_main(void *arg) {
#else
static void *server_state_watcher_main(void *arg) {
#endif
  server_state_watcher_ctx_t *ctx = (server_state_watcher_ctx_t *)arg;

  while (!shutdown_requested()) {
    char web_url[sizeof(ctx->last_web_url)];

    if (server_wait_for_refresh_interval(SERVER_STATE_REFRESH_INTERVAL_SECONDS) != 0) {
      break;
    }

    if (server_build_current_web_url(ctx->port, web_url, sizeof(web_url)) != 0) {
      web_url[0] = '\0';
    }

    if (strcmp(web_url, ctx->last_web_url) != 0) {
      if (server_persist_daemon_state(ctx->receive_dir, ctx->port,
                                      ctx->log_path[0] != '\0' ? ctx->log_path : NULL,
                                      ctx->daemon_mode,
                                      NULL, 0) != 0) {
        fprintf(stderr, "failed to refresh daemon state\n");
        continue;
      }
      fprintf(stderr, "network changed, updated web ui address to %s\n", web_url);
      (void)snprintf(ctx->last_web_url, sizeof(ctx->last_web_url), "%s", web_url);
    }
  }

#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

static int server_start_state_watcher(server_thread_t *thread_out,
                                      server_state_watcher_ctx_t **ctx_out,
                                      const char *receive_dir,
                                      uint16_t port,
                                      const char *log_path,
                                      int daemon_mode,
                                      const char *initial_web_url) {
  server_state_watcher_ctx_t *ctx = NULL;

  if (thread_out == NULL || ctx_out == NULL || receive_dir == NULL ||
      receive_dir[0] == '\0' || initial_web_url == NULL) {
    return 1;
  }

  memset(thread_out, 0, sizeof(*thread_out));
  *ctx_out = NULL;

  ctx = (server_state_watcher_ctx_t *)malloc(sizeof(*ctx));
  if (ctx == NULL) {
    perror("malloc(server_state_watcher)");
    return 1;
  }

  memset(ctx, 0, sizeof(*ctx));
  (void)snprintf(ctx->receive_dir, sizeof(ctx->receive_dir), "%s", receive_dir);
  (void)snprintf(ctx->log_path, sizeof(ctx->log_path), "%s",
                 log_path != NULL ? log_path : "");
  ctx->port = port;
  ctx->daemon_mode = daemon_mode ? 1 : 0;
  (void)snprintf(ctx->last_web_url, sizeof(ctx->last_web_url), "%s", initial_web_url);

#ifdef _WIN32
  {
    uintptr_t handle = _beginthreadex(NULL, 0, server_state_watcher_main, ctx, 0, NULL);
    if (handle == 0) {
      fprintf(stderr, "_beginthreadex(server_state_watcher) failed\n");
      free(ctx);
      return 1;
    }
    thread_out->handle = (HANDLE)handle;
  }
#else
  {
    int err = pthread_create(&thread_out->tid, NULL, server_state_watcher_main, ctx);
    if (err != 0) {
      fprintf(stderr, "pthread_create(server_state_watcher): %s\n", strerror(err));
      free(ctx);
      return 1;
    }
  }
#endif

  *ctx_out = ctx;
  return 0;
}

static void server_join_state_watcher(server_thread_t *thread,
                                      server_state_watcher_ctx_t *ctx) {
  if (ctx == NULL || thread == NULL) {
    free(ctx);
    return;
  }

#ifdef _WIN32
  if (thread->handle != NULL) {
    (void)WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    thread->handle = NULL;
  }
#else
  (void)pthread_join(thread->tid, NULL);
  memset(thread, 0, sizeof(*thread));
#endif
  free(ctx);
}

static void server_print_access_details(const server_opt_t *ser_opt,
                                        const char *log_path,
                                        long pid,
                                        int daemon_mode) {
  control_print_server_access_details(stdout, ser_opt->path, ser_opt->port,
                                      log_path, pid, daemon_mode);
}

static void server_print_shutdown_notice(void) {
  int add_leading_newline = 0;

  if (shutdown_signal_number() == SIGINT) {
#ifdef _WIN32
    add_leading_newline = _isatty(_fileno(stderr)) ? 1 : 0;
#else
    add_leading_newline = isatty(fileno(stderr)) ? 1 : 0;
#endif
  }

  if (add_leading_newline) {
    fprintf(stderr, "\nshutdown requested, stopping server\n");
  } else {
    fprintf(stderr, "shutdown requested, stopping server\n");
  }
}

static int server_notify_parent(int *ready_fd, uint8_t status) {
#ifdef _WIN32
  (void)ready_fd;
  (void)status;
  return 0;
#else
  if (ready_fd == NULL || *ready_fd < 0) {
    return 0;
  }

  ssize_t n = write(*ready_fd, &status, sizeof(status));
  int saved_errno = errno;
  (void)close(*ready_fd);
  *ready_fd = -1;
  errno = saved_errno;
  return n == (ssize_t)sizeof(status) ? 0 : 1;
#endif
}

static int server_pid_is_running_long(long pid) {
  if (pid <= 0) {
    return 0;
  }

#ifdef _WIN32
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
  DWORD wait_res = WAIT_FAILED;
  if (process == NULL) {
    return 0;
  }
  wait_res = WaitForSingleObject(process, 0);
  CloseHandle(process);
  return wait_res == WAIT_TIMEOUT ? 1 : 0;
#else
  if (kill((pid_t)pid, 0) == 0) {
    return 1;
  }
  return errno == EPERM ? 1 : 0;
#endif
}

static int server_prepare_state_files(void) {
  daemon_state_t state = {0};

  if (daemon_state_read(&state) == 0 && server_pid_is_running_long(state.pid)) {
    fprintf(stderr, "HFile is already running\nrun 'hf status' or 'hf stop'\n");
    return 1;
  }

  daemon_state_cleanup_files();
  return 0;
}

#ifndef _WIN32

static int server_load_pid_file(const char *pid_path, pid_t *pid_out) {
  FILE *fp = NULL;
  long value = 0;

  if (pid_out == NULL) {
    return 1;
  }
  *pid_out = 0;

  fp = fopen(pid_path, "r");
  if (fp == NULL) {
    if (errno == ENOENT) {
      return 0;
    }
    perror("fopen(pid_file)");
    return 1;
  }

  if (fscanf(fp, "%ld", &value) != 1) {
    value = 0;
  }
  (void)fclose(fp);

  if (value > 0) {
    *pid_out = (pid_t)value;
  }
  return 0;
}

static int server_prepare_pid_file(const char *pid_path) {
  pid_t existing_pid = 0;

  if (server_load_pid_file(pid_path, &existing_pid) != 0) {
    return 1;
  }
  if (server_pid_is_running_long((long)existing_pid)) {
    fprintf(stderr, "HFile is already running\nrun 'hf status' or 'hf stop'\n");
    return 1;
  }
  return server_prepare_state_files();
}

static int server_write_pid_file(const char *pid_path, pid_t pid) {
  FILE *fp = fopen(pid_path, "w");
  if (fp == NULL) {
    perror("fopen(pid_file)");
    return 1;
  }
  if (fprintf(fp, "%ld\n", (long)pid) < 0) {
    perror("fprintf(pid_file)");
    (void)fclose(fp);
    return 1;
  }
  if (fclose(fp) != 0) {
    perror("fclose(pid_file)");
    return 1;
  }
  return 0;
}

static int server_redirect_stdio(const char *log_path) {
  int err_fd = -1;
  int null_fd = -1;

  err_fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (err_fd < 0) {
    perror("open(error_log)");
    return 1;
  }

  null_fd = open("/dev/null", O_RDONLY);
  if (null_fd < 0) {
    perror("open(/dev/null)");
    (void)close(err_fd);
    return 1;
  }

  if (dup2(null_fd, STDIN_FILENO) < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
      dup2(err_fd, STDERR_FILENO) < 0) {
    perror("dup2(stdio)");
    (void)close(null_fd);
    (void)close(err_fd);
    return 1;
  }

  (void)close(null_fd);
  (void)close(err_fd);
  return 0;
}

static int server_wait_for_daemon_ready(int ready_fd, pid_t child_pid) {
  uint8_t status = 0;
  ssize_t n = read(ready_fd, &status, sizeof(status));
  int saved_errno = errno;
  int wait_status = 0;

  (void)close(ready_fd);

  if (n == (ssize_t)sizeof(status) && status == 1u) {
    return 0;
  }

  if (n < 0) {
    errno = saved_errno;
    perror("read(daemon_ready)");
  }

  (void)waitpid(child_pid, &wait_status, 0);
  return 1;
}
#endif

static int server_run_process(const server_opt_t *ser_opt,
                              int ready_fd,
                              const char *log_path,
                              int daemon_mode,
                              int persist_state) {
  int exit_code = 0;
  int ready_notified = 0;
  server_thread_t state_watcher_thread = {0};
  server_state_watcher_ctx_t *state_watcher_ctx = NULL;

  if (ser_opt->path == NULL || strlen(ser_opt->path) == 0) {
    exit_code = 1;
    goto CLEAN_UP;
  }

  if (message_store_init() != 0) {
    fprintf(stderr, "failed to initialize message store\n");
    exit_code = 1;
    goto CLEAN_UP;
  }
  if (server_conn_tracker_init() != 0) {
    fprintf(stderr, "failed to initialize connection tracker\n");
    exit_code = 1;
    goto CLEAN_UP;
  }

  socket_t sock;
  socket_init(&sock);

  if (create_listener_socket(NULL, ser_opt->port, &sock) != 0) {
    exit_code = 1;
    goto CLOSE_SOCK;
  }

  if (persist_state) {
    char initial_web_url[256];

    if (server_persist_daemon_state(ser_opt->path, ser_opt->port, log_path,
                                    daemon_mode,
                                    initial_web_url, sizeof(initial_web_url)) != 0) {
      fprintf(stderr, "failed to persist daemon state\n");
      exit_code = 1;
      goto CLOSE_SOCK;
    }

    if (server_start_state_watcher(&state_watcher_thread, &state_watcher_ctx,
                                   ser_opt->path, ser_opt->port, log_path,
                                   daemon_mode, initial_web_url) != 0) {
      fprintf(stderr, "failed to start daemon state watcher\n");
    }
  }

  if (ready_fd >= 0) {
    if (server_notify_parent(&ready_fd, 1u) != 0) {
      exit_code = 1;
      goto CLOSE_SOCK;
    }
    ready_notified = 1;
  }

  server_print_access_details(ser_opt, log_path, server_current_pid_long(), daemon_mode);

  exit_code = server_run_listener(sock, ser_opt);
  if (shutdown_requested()) {
    server_print_shutdown_notice();
  }

CLOSE_SOCK:
  shutdown_request();
  server_join_state_watcher(&state_watcher_thread, state_watcher_ctx);
  state_watcher_ctx = NULL;
  message_store_shutdown();

  socket_close(sock);
  server_conn_tracker_shutdown_all();
  server_conn_tracker_wait_idle();

CLEAN_UP:
  if (ready_fd >= 0 && !ready_notified) {
    (void)server_notify_parent(&ready_fd, 0u);
  }
  if (persist_state) {
    daemon_state_cleanup_files();
  }
  message_store_cleanup();
  server_conn_tracker_cleanup();
  return exit_code;
}

int server(const server_opt_t *ser_opt) {
#ifdef _WIN32
  if (ser_opt == NULL) {
    fprintf(stderr, "invalid server options\n");
    return 1;
  }
  if (server_prepare_state_files() != 0) {
    return 1;
  }
  fprintf(stderr,
          "daemon mode is not supported on Windows; running attached server instead\n");
  return server_run_process(ser_opt, -1, NULL, 0, 1);
#else
  char log_path[4096];
  char pid_path[4096];

  if (ser_opt == NULL) {
    fprintf(stderr, "invalid server options\n");
    return 1;
  }

  if (daemon_state_default_log_path(log_path, sizeof(log_path)) != 0) {
    fprintf(stderr, "invalid log file path\n");
    return 1;
  }
  if (daemon_state_default_pid_path(pid_path, sizeof(pid_path)) != 0) {
    fprintf(stderr, "invalid pid file path\n");
    return 1;
  }
  if (server_prepare_pid_file(pid_path) != 0) {
    return 1;
  }

  int ready_pipe[2] = {-1, -1};
  if (pipe(ready_pipe) != 0) {
    perror("pipe(daemon_ready)");
    return 1;
  }

  pid_t child_pid = fork();
  if (child_pid < 0) {
    perror("fork");
    (void)close(ready_pipe[0]);
    (void)close(ready_pipe[1]);
    return 1;
  }

  if (child_pid > 0) {
    (void)close(ready_pipe[1]);
    if (server_wait_for_daemon_ready(ready_pipe[0], child_pid) != 0) {
      fprintf(stderr, "failed to start daemon, see %s\n", log_path);
      return 1;
    }

    server_print_access_details(ser_opt, log_path, (long)child_pid, 1);
    return 0;
  }

  (void)close(ready_pipe[0]);
  if (setsid() < 0) {
    perror("setsid");
    (void)server_notify_parent(&ready_pipe[1], 0u);
    return 1;
  }
  if (server_redirect_stdio(log_path) != 0) {
    (void)server_notify_parent(&ready_pipe[1], 0u);
    return 1;
  }
  if (server_write_pid_file(pid_path, getpid()) != 0) {
    (void)server_notify_parent(&ready_pipe[1], 0u);
    return 1;
  }

  server_opt_t child_opt = *ser_opt;
  int exit_code = server_run_process(&child_opt, ready_pipe[1], log_path, 1, 1);
  return exit_code;
#endif
}
