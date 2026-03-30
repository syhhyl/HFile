#include "cli.h"
#include "daemon_state.h"
#include "http.h"
#include "message_store.h"
#include "mobile_ui.h"
#include "net.h"
#include "protocol.h"
#include "shutdown.h"
#include "server.h"
#include "transfer_io.h"

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

static server_conn_tracker_t g_server_conn_tracker = {0};

typedef enum {
  SERVER_REPLY_ACK_SUCCESS = 0,
  SERVER_REPLY_ACK_FAILURE = 1,
  SERVER_REPLY_CLOSE = 2
} server_reply_t;

static int server_handle_text_message(socket_t conn,
                                      const protocol_header_t *proto_header,
                                      uint8_t *ack);
static int server_handle_file_transfer(socket_t conn,
                                       const server_opt_t *ser_opt,
                                       const protocol_header_t *proto_header,
                                       server_reply_t *reply);

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

#ifdef _WIN32
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == INVALID_SOCKET) {
#else
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
#endif
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

static int server_handle_protocol_connection(socket_t conn,
                                             const server_opt_t *ser_opt) {
  int exit_code = 1;
  ssize_t ack_sent = 0;
  server_reply_t reply = SERVER_REPLY_ACK_FAILURE;
  uint8_t header_buf[HF_PROTOCOL_HEADER_SIZE];
  protocol_header_t proto_header = {0};

  protocol_result_t header_res = recv_header(conn, header_buf);
  if (header_res != PROTOCOL_OK) {
    if (header_res == PROTOCOL_ERR_EOF) {
      fprintf(stderr, "protocol error: unexpected EOF while receiving header\n");
    } else {
      sock_perror("recv_header");
    }
    return 1;
  }

  header_res = decode_header(&proto_header, header_buf);
  if (header_res != PROTOCOL_OK) {
    fprintf(stderr, "protocol error: failed to decode header\n");
    goto SEND_REPLY;
  }

  switch (proto_header.msg_type) {
    case HF_MSG_TYPE_TEXT_MESSAGE: {
      uint8_t ack = 1;
      exit_code = server_handle_text_message(conn, &proto_header, &ack);
      reply = (ack == 0) ? SERVER_REPLY_ACK_SUCCESS : SERVER_REPLY_ACK_FAILURE;
      break;
    }

    case HF_MSG_TYPE_FILE_TRANSFER:
      exit_code = server_handle_file_transfer(conn, ser_opt, &proto_header, &reply);
      break;

    default:
      fprintf(stderr, "protocol error: unsupported message type: %u\n",
              (unsigned)proto_header.msg_type);
      exit_code = 1;
      break;
  }

SEND_REPLY:
  if (reply != SERVER_REPLY_CLOSE) {
    uint8_t ack = (reply == SERVER_REPLY_ACK_SUCCESS) ? 0 : 1;
    ack_sent = send_all(conn, &ack, sizeof(ack));
    if (ack_sent != (ssize_t)sizeof(ack)) {
      sock_perror("send_all(ack)");
    }
  }

  return exit_code;
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
      (void)http_handle_connection(conn, &opt);
      break;
    case SERVER_CONN_KIND_PROTOCOL:
      (void)server_handle_protocol_connection(conn, &opt);
      break;
    default:
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
                                      const protocol_header_t *proto_header,
                                      uint8_t *ack) {
  char *message = NULL;

  if (ack == NULL) {
    fprintf(stderr, "invalid text message handler arguments\n");
    return 1;
  }

  if (proto_header->flags != HF_MSG_FLAG_NONE) {
    fprintf(stderr, "protocol error: text message has unsupported flags\n");
    return 1;
  }

  if (proto_header->payload_size > HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE) {
    fprintf(stderr, "protocol error: text message too large\n");
    return 1;
  }

  size_t message_len = (size_t)proto_header->payload_size;
  message = (char *)malloc(message_len + 1u);
  if (message == NULL) {
    perror("malloc(message)");
    return 1;
  }

  if (message_len > 0) {
    ssize_t n = recv_all(conn, message, message_len);
    if (n != (ssize_t)message_len) {
      free(message);
      if (n < 0) {
        sock_perror("recv_all(message)");
      } else {
        fprintf(stderr,
                "protocol error: unexpected EOF while receiving message\n");
      }
      return 1;
    }
  }

  message[message_len] = '\0';
  if (message_store_set(message) != 0) {
    fprintf(stderr, "failed to store latest message\n");
    free(message);
    return 1;
  }
  free(message);
  *ack = 0;
  return 0;
}

static int server_handle_file_transfer(socket_t conn,
                                       const server_opt_t *ser_opt,
                                       const protocol_header_t *proto_header,
                                       server_reply_t *reply) {
  int exit_code = 1;
  char *file_name = NULL;
  char full_path[4096];
  uint64_t content_size = 0;
  uint64_t prefix_size = 0;

  if (reply == NULL || ser_opt == NULL) {
    fprintf(stderr, "invalid file transfer handler arguments\n");
    return 1;
  }

  *reply = SERVER_REPLY_ACK_FAILURE;

  if (proto_header->payload_size <
      (uint64_t)proto_file_transfer_prefix_size(0)) {
    fprintf(stderr, "protocol error: payload size too small\n");
    return 1;
  }

  protocol_result_t header_res = proto_recv_file_transfer_prefix(conn,
    &file_name, &content_size);
  if (header_res != PROTOCOL_OK) {
    if (header_res == PROTOCOL_ERR_FILE_NAME_LEN) {
      fprintf(stderr, "protocol error: invalid file name length\n");
    } else if (header_res == PROTOCOL_ERR_ALLOC) {
      perror("malloc(file_name)");
    } else if (header_res == PROTOCOL_ERR_EOF) {
      fprintf(stderr,
              "protocol error: unexpected EOF while receiving payload\n");
    } else {
      sock_perror("protocol_recv_file_transfer_prefix");
    }
    goto CLEANUP;
  }

  if (fs_validate_file_name(file_name) != 0) {
    fprintf(stderr, "invalid file name: %s\n", file_name);
    goto CLEANUP;
  }

  prefix_size = (uint64_t)proto_file_transfer_prefix_size((uint16_t)strlen(file_name));
  if (proto_header->payload_size != prefix_size + content_size) {
    fprintf(stderr, "protocol error: payload size mismatch\n");
    goto CLEANUP;
  }

  uint8_t ready_ack = 0;
  ssize_t ready_ack_sent = send_all(conn, &ready_ack, sizeof(ready_ack));
  if (ready_ack_sent != (ssize_t)sizeof(ready_ack)) {
    sock_perror("send_all(file_transfer_ready_ack)");
    *reply = SERVER_REPLY_CLOSE;
    goto CLEANUP;
  }
  *reply = SERVER_REPLY_ACK_FAILURE;

  if (transfer_recv_socket_file(conn, ser_opt->path, file_name, content_size,
                                "recv(file_body)",
                                "protocol error: unexpected EOF while receiving file",
                                full_path, sizeof(full_path)) != 0) {
    goto CLEANUP;
  }

  *reply = SERVER_REPLY_ACK_SUCCESS;
  exit_code = 0;

CLEANUP:
  if (file_name != NULL) free(file_name);
  return exit_code;
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

#ifdef _WIN32
    SOCKET conn = accept(sock, NULL, NULL);
    if (conn == INVALID_SOCKET) {
      if (WSAGetLastError() == WSAEINTR) continue;
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
#else
    int conn = accept(sock, NULL, NULL);
    if (conn < 0) {
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

static void server_print_access_details(const server_opt_t *ser_opt,
                                        const char *log_path,
                                        long pid,
                                        int daemon_mode) {
  char url[256];
  int phone_reachable = 0;

  printf(daemon_mode ? "HFile daemon ready\n" : "HFile server ready\n");
  printf("  receive dir: %s\n", ser_opt->path);
  printf("  listen     : 0.0.0.0:%u (tcp + web ui)\n", (unsigned)ser_opt->port);
  if (daemon_mode) {
    printf("  pid        : %ld\n", pid);
    if (log_path != NULL) {
      printf("  error log  : %s\n", log_path);
    }
  }

  if (mobile_ui_build_url(ser_opt->port, url, sizeof(url), &phone_reachable) == 0) {
    printf("  web ui     : %s\n", url);
    if (!phone_reachable) {
      printf("  mobile     : could not detect a LAN IPv4, using localhost\n");
    }
    if (mobile_ui_is_tty_stdout()) {
      (void)mobile_ui_print_qr(stdout, url);
    }
  }

  if (!daemon_mode) {
    printf("  status     : waiting for files and messages\n");
  }

  fflush(stdout);
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

#ifndef _WIN32
static int server_pid_is_running(pid_t pid) {
  if (pid <= 0) {
    return 0;
  }
  if (kill(pid, 0) == 0) {
    return 1;
  }
  return errno == EPERM ? 1 : 0;
}

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
  if (server_pid_is_running(existing_pid)) {
    fprintf(stderr, "HFile is already running\nrun 'hf status' or 'hf stop'\n");
    return 1;
  }
  daemon_state_cleanup_files();
  return 0;
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
                              int daemon_mode) {
  int exit_code = 0;
  int ready_notified = 0;

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

  if (daemon_mode) {
    daemon_state_t state = {0};
    state.pid = server_current_pid_long();
    (void)snprintf(state.receive_dir, sizeof(state.receive_dir), "%s", ser_opt->path);
    (void)snprintf(state.log_path, sizeof(state.log_path), "%s",
                   log_path != NULL ? log_path : "");
    state.port = ser_opt->port;
    state.daemon_mode = 1;
    {
      int phone_reachable = 0;
      if (mobile_ui_build_url(ser_opt->port, state.web_url, sizeof(state.web_url),
                              &phone_reachable) != 0) {
        state.web_url[0] = '\0';
      }
    }

    if (daemon_state_write(&state) != 0) {
      fprintf(stderr, "failed to persist daemon state\n");
      exit_code = 1;
      goto CLOSE_SOCK;
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
  message_store_shutdown();

  socket_close(sock);
  server_conn_tracker_shutdown_all();
  server_conn_tracker_wait_idle();

CLEAN_UP:
  if (ready_fd >= 0 && !ready_notified) {
    (void)server_notify_parent(&ready_fd, 0u);
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
  if (ser_opt->daemonize) {
    fprintf(stderr, "daemon mode is not supported on Windows\n");
    return 1;
  }
  return server_run_process(ser_opt, -1, NULL, 0);
#else
  char log_path[4096];
  char pid_path[4096];

  if (ser_opt == NULL) {
    fprintf(stderr, "invalid server options\n");
    return 1;
  }

  if (!ser_opt->daemonize) {
    return server_run_process(ser_opt, -1, NULL, 0);
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
  child_opt.daemonize = 0;
  int exit_code = server_run_process(&child_opt, ready_pipe[1], log_path, 1);
  daemon_state_cleanup_files();
  return exit_code;
#endif
}
