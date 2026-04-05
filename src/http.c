#include "http.h"

#include "fs.h"
#include "message_store.h"
#include "net.h"
#include "protocol.h"
#include "shutdown.h"
#include "transfer_io.h"
#include "webui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <io.h>
  #include <process.h>
  #include <sys/stat.h>
  #include <windows.h>
#define strtok_r strtok_s
#else
  #include <dirent.h>
  #include <pthread.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#define HF_HTTP_HEADER_MAX 16384u
#define HF_HTTP_PATH_MAX 1024u
#define HF_HTTP_CONTENT_TYPE_MAX 128u
#define HF_HTTP_UPLOAD_MAX (16ULL * 1024ULL * 1024ULL * 1024ULL)

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} http_buf_t;

typedef struct {
  char method[8];
  char path[HF_HTTP_PATH_MAX];
  char content_type[HF_HTTP_CONTENT_TYPE_MAX];
  uint64_t content_length;
  int has_content_length;
} http_request_t;

typedef struct {
  char *name;
  uint64_t size;
  uint64_t mtime;
} http_file_entry_t;

typedef struct http_conn_entry_t {
  socket_t conn;
  int closing;
  struct http_conn_entry_t *next;
} http_conn_entry_t;

typedef struct {
  socket_t conn;
  server_opt_t opt;
  http_conn_entry_t *entry;
} http_conn_ctx_t;

typedef struct {
  int initialized;
  int shutting_down;
  unsigned int active_count;
  http_conn_entry_t *head;
#ifdef _WIN32
  CRITICAL_SECTION mutex;
  CONDITION_VARIABLE cond;
#else
  pthread_mutex_t mutex;
  pthread_cond_t cond;
#endif
} http_conn_tracker_t;

static http_conn_tracker_t g_http_conn_tracker = {0};

static int http_conn_tracker_init(void) {
  if (g_http_conn_tracker.initialized) {
    return 0;
  }

#ifdef _WIN32
  InitializeCriticalSection(&g_http_conn_tracker.mutex);
  InitializeConditionVariable(&g_http_conn_tracker.cond);
#else
  if (pthread_mutex_init(&g_http_conn_tracker.mutex, NULL) != 0) {
    return 1;
  }
  if (pthread_cond_init(&g_http_conn_tracker.cond, NULL) != 0) {
    (void)pthread_mutex_destroy(&g_http_conn_tracker.mutex);
    return 1;
  }
#endif

  g_http_conn_tracker.initialized = 1;
  g_http_conn_tracker.shutting_down = 0;
  g_http_conn_tracker.active_count = 0;
  g_http_conn_tracker.head = NULL;
  return 0;
}

static void http_conn_tracker_abort_entry(http_conn_entry_t *entry) {
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

static http_conn_entry_t *http_conn_tracker_begin(socket_t conn) {
  http_conn_entry_t *entry = (http_conn_entry_t *)malloc(sizeof(*entry));
  if (entry == NULL) {
    return NULL;
  }

  entry->conn = conn;
  entry->closing = 0;
  entry->next = NULL;

#ifdef _WIN32
  EnterCriticalSection(&g_http_conn_tracker.mutex);
#else
  (void)pthread_mutex_lock(&g_http_conn_tracker.mutex);
#endif
  if (g_http_conn_tracker.shutting_down) {
#ifdef _WIN32
    LeaveCriticalSection(&g_http_conn_tracker.mutex);
#else
    (void)pthread_mutex_unlock(&g_http_conn_tracker.mutex);
#endif
    free(entry);
    return NULL;
  }

  entry->next = g_http_conn_tracker.head;
  g_http_conn_tracker.head = entry;
  g_http_conn_tracker.active_count++;
#ifdef _WIN32
  LeaveCriticalSection(&g_http_conn_tracker.mutex);
#else
  (void)pthread_mutex_unlock(&g_http_conn_tracker.mutex);
#endif
  return entry;
}

static void http_conn_tracker_end(http_conn_entry_t *entry) {
#ifdef _WIN32
  EnterCriticalSection(&g_http_conn_tracker.mutex);
#else
  (void)pthread_mutex_lock(&g_http_conn_tracker.mutex);
#endif

  http_conn_entry_t **link = &g_http_conn_tracker.head;
  while (*link != NULL) {
    if (*link == entry) {
      *link = entry->next;
      break;
    }
    link = &(*link)->next;
  }
  if (g_http_conn_tracker.active_count > 0) {
    g_http_conn_tracker.active_count--;
  }
  if (g_http_conn_tracker.shutting_down && g_http_conn_tracker.active_count == 0) {
#ifdef _WIN32
    WakeAllConditionVariable(&g_http_conn_tracker.cond);
#else
    (void)pthread_cond_broadcast(&g_http_conn_tracker.cond);
#endif
  }
#ifdef _WIN32
  LeaveCriticalSection(&g_http_conn_tracker.mutex);
#else
  (void)pthread_mutex_unlock(&g_http_conn_tracker.mutex);
#endif

  if (entry != NULL) {
    free(entry);
  }
}

static void http_conn_tracker_shutdown(void) {
#ifdef _WIN32
  EnterCriticalSection(&g_http_conn_tracker.mutex);
#else
  (void)pthread_mutex_lock(&g_http_conn_tracker.mutex);
#endif
  g_http_conn_tracker.shutting_down = 1;
  for (http_conn_entry_t *entry = g_http_conn_tracker.head;
       entry != NULL;
       entry = entry->next) {
    http_conn_tracker_abort_entry(entry);
  }
  if (g_http_conn_tracker.active_count == 0) {
#ifdef _WIN32
    WakeAllConditionVariable(&g_http_conn_tracker.cond);
#else
    (void)pthread_cond_broadcast(&g_http_conn_tracker.cond);
#endif
  }
#ifdef _WIN32
  LeaveCriticalSection(&g_http_conn_tracker.mutex);
#else
  (void)pthread_mutex_unlock(&g_http_conn_tracker.mutex);
#endif
}

static void http_conn_tracker_wait_idle(void) {
#ifdef _WIN32
  EnterCriticalSection(&g_http_conn_tracker.mutex);
  while (g_http_conn_tracker.active_count > 0) {
    SleepConditionVariableCS(&g_http_conn_tracker.cond, &g_http_conn_tracker.mutex,
                             INFINITE);
  }
  LeaveCriticalSection(&g_http_conn_tracker.mutex);
#else
  (void)pthread_mutex_lock(&g_http_conn_tracker.mutex);
  while (g_http_conn_tracker.active_count > 0) {
    (void)pthread_cond_wait(&g_http_conn_tracker.cond, &g_http_conn_tracker.mutex);
  }
  (void)pthread_mutex_unlock(&g_http_conn_tracker.mutex);
#endif
}

static void http_conn_tracker_cleanup(void) {
  if (!g_http_conn_tracker.initialized) {
    return;
  }

#ifdef _WIN32
  DeleteCriticalSection(&g_http_conn_tracker.mutex);
#else
  (void)pthread_cond_destroy(&g_http_conn_tracker.cond);
  (void)pthread_mutex_destroy(&g_http_conn_tracker.mutex);
#endif

  g_http_conn_tracker.initialized = 0;
  g_http_conn_tracker.shutting_down = 0;
  g_http_conn_tracker.active_count = 0;
  g_http_conn_tracker.head = NULL;
}

static int http_buf_reserve(http_buf_t *buf, size_t need) {
  if (buf->cap >= need) {
    return 0;
  }

  size_t new_cap = buf->cap == 0 ? 256u : buf->cap;
  while (new_cap < need) {
    if (new_cap > (SIZE_MAX / 2u)) {
      return 1;
    }
    new_cap *= 2u;
  }

  char *new_data = (char *)realloc(buf->data, new_cap);
  if (new_data == NULL) {
    return 1;
  }

  buf->data = new_data;
  buf->cap = new_cap;
  return 0;
}

static int http_buf_append(http_buf_t *buf, const char *data, size_t len) {
  if (http_buf_reserve(buf, buf->len + len + 1u) != 0) {
    return 1;
  }

  memcpy(buf->data + buf->len, data, len);
  buf->len += len;
  buf->data[buf->len] = '\0';
  return 0;
}

static int http_buf_append_str(http_buf_t *buf, const char *s) {
  return http_buf_append(buf, s, strlen(s));
}

static int http_buf_append_ch(http_buf_t *buf, char ch) {
  return http_buf_append(buf, &ch, 1u);
}

static void http_buf_free(http_buf_t *buf) {
  if (buf->data != NULL) {
    free(buf->data);
  }
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

static int http_ascii_stricmp(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    int ca = tolower((unsigned char)*a);
    int cb = tolower((unsigned char)*b);
    if (ca != cb) {
      return ca - cb;
    }
    a++;
    b++;
  }
  return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static int http_ascii_starts_with(const char *s, const char *prefix) {
  while (*prefix != '\0') {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
      return 0;
    }
    s++;
    prefix++;
  }
  return 1;
}

static int http_parse_u64(const char *s, uint64_t *out) {
  uint64_t value = 0;

  if (s == NULL || *s == '\0' || out == NULL) {
    return 1;
  }

  while (*s != '\0') {
    unsigned char ch = (unsigned char)*s;
    if (!isdigit(ch)) {
      return 1;
    }
    if (value > (UINT64_MAX - (uint64_t)(ch - '0')) / 10u) {
      return 1;
    }
    value = value * 10u + (uint64_t)(ch - '0');
    s++;
  }

  *out = value;
  return 0;
}

static int http_discard_body(socket_t conn, uint64_t content_length) {
  char buf[4096];
  uint64_t remaining = content_length;

  while (remaining > 0) {
    size_t want = sizeof(buf);
    if ((uint64_t)want > remaining) {
      want = (size_t)remaining;
    }

    ssize_t n = recv(conn, buf, want, 0);
    if (n < 0) {
      sock_perror("recv(http_discard_body)");
      return 1;
    }
    if (n == 0) {
      return 1;
    }
    remaining -= (uint64_t)n;
  }

  return 0;
}

static int http_send_response(socket_t conn,
                              int status,
                              const char *reason,
                              const char *content_type,
                              const void *body,
                              size_t body_len,
                              const char *extra_headers) {
  char header[1024];
  int n = snprintf(header, sizeof(header),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Connection: close\r\n"
                   "%s"
                   "\r\n",
                   status, reason, content_type, body_len,
                   extra_headers != NULL ? extra_headers : "");
  if (n < 0 || (size_t)n >= sizeof(header)) {
    return 1;
  }

  if (send_all(conn, header, (size_t)n) != (ssize_t)n) {
    return 1;
  }

  if (body_len > 0 &&
      send_all(conn, body, body_len) != (ssize_t)body_len) {
    return 1;
  }

  return 0;
}

static int http_send_sse_headers(socket_t conn) {
  static const char header[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream; charset=utf-8\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: close\r\n"
    "X-Accel-Buffering: no\r\n"
    "\r\n";

  return send_all(conn, header, sizeof(header) - 1u) ==
         (ssize_t)(sizeof(header) - 1u)
           ? 0
           : 1;
}

static int http_send_sse_message_event(socket_t conn, const char *message) {
  http_buf_t event = {0};
  int exit_code = 1;

  if (http_buf_append_str(&event, "event: message\n") != 0 ||
      http_buf_append_str(&event, "data: ") != 0) {
    goto CLEANUP;
  }

  if (message != NULL) {
    const char *line = message;
    const char *next = NULL;

    for (;;) {
      next = strchr(line, '\n');
      if (next == NULL) {
        if (http_buf_append(&event, line, strlen(line)) != 0) {
          goto CLEANUP;
        }
        break;
      }
      if (http_buf_append(&event, line, (size_t)(next - line)) != 0 ||
          http_buf_append_str(&event, "\ndata: ") != 0) {
        goto CLEANUP;
      }
      line = next + 1;
    }
  }

  if (http_buf_append_str(&event, "\n\n") != 0) {
    goto CLEANUP;
  }

  if (send_all(conn, event.data, event.len) != (ssize_t)event.len) {
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  http_buf_free(&event);
  return exit_code;
}

static int http_send_sse_keepalive(socket_t conn) {
  static const char keepalive[] = ": keep-alive\n\n";
  return send_all(conn, keepalive, sizeof(keepalive) - 1u) ==
         (ssize_t)(sizeof(keepalive) - 1u)
           ? 0
           : 1;
}

static int http_send_json_error(socket_t conn, int status, const char *reason,
                                const char *message) {
  http_buf_t body = {0};
  int exit_code = 1;

  if (http_buf_append_str(&body, "{\"error\":\"") != 0) {
    goto CLEANUP;
  }

  for (const unsigned char *p = (const unsigned char *)message; *p != '\0'; p++) {
    char escaped[7];
    switch (*p) {
      case '\\':
        if (http_buf_append_str(&body, "\\\\") != 0) goto CLEANUP;
        break;
      case '"':
        if (http_buf_append_str(&body, "\\\"") != 0) goto CLEANUP;
        break;
      case '\n':
        if (http_buf_append_str(&body, "\\n") != 0) goto CLEANUP;
        break;
      case '\r':
        if (http_buf_append_str(&body, "\\r") != 0) goto CLEANUP;
        break;
      case '\t':
        if (http_buf_append_str(&body, "\\t") != 0) goto CLEANUP;
        break;
      default:
        if (*p < 0x20u) {
          int n = snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
          if (n < 0 || http_buf_append(&body, escaped, (size_t)n) != 0) {
            goto CLEANUP;
          }
        } else if (http_buf_append_ch(&body, (char)*p) != 0) {
          goto CLEANUP;
        }
        break;
    }
  }

  if (http_buf_append_str(&body, "\"}") != 0) {
    goto CLEANUP;
  }

  if (http_send_response(conn, status, reason, "application/json; charset=utf-8",
                         body.data, body.len, NULL) != 0) {
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  http_buf_free(&body);
  return exit_code;
}

static int http_read_header_block(socket_t conn, char *out, size_t out_cap) {
  size_t len = 0;

  if (out == NULL || out_cap < 5u) {
    return 1;
  }

  while (len + 1u < out_cap) {
    char ch = '\0';
    ssize_t n = recv(conn, &ch, 1, 0);
    if (n < 0) {
      return -1;
    }
    if (n == 0) {
      return 1;
    }
    out[len++] = ch;
    out[len] = '\0';
    if (len >= 4u &&
        out[len - 4u] == '\r' &&
        out[len - 3u] == '\n' &&
        out[len - 2u] == '\r' &&
        out[len - 1u] == '\n') {
      return 0;
    }
  }

  return 2;
}

static char *http_trim(char *s) {
  char *end = NULL;

  while (*s != '\0' && isspace((unsigned char)*s)) {
    s++;
  }

  end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1])) {
    end--;
  }
  *end = '\0';
  return s;
}

static int http_parse_request(char *header_block, http_request_t *req) {
  char *line = NULL;
  char *save = NULL;
  char *method = NULL;
  char *path = NULL;
  char *version = NULL;

  if (header_block == NULL || req == NULL) {
    return 1;
  }

  memset(req, 0, sizeof(*req));

  line = strtok_r(header_block, "\r\n", &save);
  if (line == NULL) {
    return 1;
  }

  method = strtok(line, " ");
  path = strtok(NULL, " ");
  version = strtok(NULL, " ");
  if (method == NULL || path == NULL || version == NULL ||
      strtok(NULL, " ") != NULL) {
    return 1;
  }

  if (strcmp(version, "HTTP/1.1") != 0) {
    return 2;
  }

  if (strlen(method) >= sizeof(req->method) ||
      strlen(path) >= sizeof(req->path)) {
    return 1;
  }

  memcpy(req->method, method, strlen(method) + 1u);
  memcpy(req->path, path, strlen(path) + 1u);

  for (;;) {
    char *name = NULL;
    char *value = NULL;
    line = strtok_r(NULL, "\r\n", &save);
    if (line == NULL) {
      break;
    }
    if (*line == '\0') {
      continue;
    }

    value = strchr(line, ':');
    if (value == NULL) {
      return 1;
    }
    *value++ = '\0';
    name = http_trim(line);
    value = http_trim(value);

    if (http_ascii_stricmp(name, "Content-Length") == 0) {
      if (http_parse_u64(value, &req->content_length) != 0) {
        return 1;
      }
      req->has_content_length = 1;
    } else if (http_ascii_stricmp(name, "Content-Type") == 0) {
      if (strlen(value) >= sizeof(req->content_type)) {
        return 1;
      }
      memcpy(req->content_type, value, strlen(value) + 1u);
    }
  }

  char *query = strchr(req->path, '?');
  if (query != NULL) {
    *query = '\0';
  }

  return 0;
}

static int http_decode_name(const char *encoded, char *out, size_t out_cap) {
  size_t oi = 0;

  if (encoded == NULL || out == NULL || out_cap == 0u) {
    return 1;
  }

  while (*encoded != '\0') {
    unsigned char ch = (unsigned char)*encoded++;
    if (oi + 1u >= out_cap) {
      return 1;
    }

    if (ch == '%') {
      int hi = 0;
      int lo = 0;
      if (!isxdigit((unsigned char)encoded[0]) ||
          !isxdigit((unsigned char)encoded[1])) {
        return 1;
      }
      hi = isdigit((unsigned char)encoded[0]) ? encoded[0] - '0'
                                              : 10 + tolower((unsigned char)encoded[0]) - 'a';
      lo = isdigit((unsigned char)encoded[1]) ? encoded[1] - '0'
                                              : 10 + tolower((unsigned char)encoded[1]) - 'a';
      out[oi++] = (char)((hi << 4) | lo);
      encoded += 2;
    } else {
      out[oi++] = (char)ch;
    }
  }

  out[oi] = '\0';
  return 0;
}

static int http_json_escape(http_buf_t *buf, const char *text) {
  for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
    char escaped[7];
    switch (*p) {
      case '\\':
        if (http_buf_append_str(buf, "\\\\") != 0) return 1;
        break;
      case '"':
        if (http_buf_append_str(buf, "\\\"") != 0) return 1;
        break;
      case '\b':
        if (http_buf_append_str(buf, "\\b") != 0) return 1;
        break;
      case '\f':
        if (http_buf_append_str(buf, "\\f") != 0) return 1;
        break;
      case '\n':
        if (http_buf_append_str(buf, "\\n") != 0) return 1;
        break;
      case '\r':
        if (http_buf_append_str(buf, "\\r") != 0) return 1;
        break;
      case '\t':
        if (http_buf_append_str(buf, "\\t") != 0) return 1;
        break;
      default:
        if (*p < 0x20u) {
          int n = snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
          if (n < 0 || http_buf_append(buf, escaped, (size_t)n) != 0) {
            return 1;
          }
        } else if (http_buf_append_ch(buf, (char)*p) != 0) {
          return 1;
        }
        break;
    }
  }
  return 0;
}

static const char *http_json_skip_ws(const char *p) {
  while (*p != '\0' && isspace((unsigned char)*p)) {
    p++;
  }
  return p;
}

static int http_json_parse_string(const char **p_in, char **out) {
  http_buf_t buf = {0};
  const char *p = *p_in;
  int exit_code = 1;

  if (*p != '"') {
    return 1;
  }
  p++;

  while (*p != '\0' && *p != '"') {
    unsigned char ch = (unsigned char)*p++;
    if (ch == '\\') {
      ch = (unsigned char)*p++;
      switch (ch) {
        case '"':
        case '\\':
        case '/':
          if (http_buf_append_ch(&buf, (char)ch) != 0) goto CLEANUP;
          break;
        case 'b':
          if (http_buf_append_ch(&buf, '\b') != 0) goto CLEANUP;
          break;
        case 'f':
          if (http_buf_append_ch(&buf, '\f') != 0) goto CLEANUP;
          break;
        case 'n':
          if (http_buf_append_ch(&buf, '\n') != 0) goto CLEANUP;
          break;
        case 'r':
          if (http_buf_append_ch(&buf, '\r') != 0) goto CLEANUP;
          break;
        case 't':
          if (http_buf_append_ch(&buf, '\t') != 0) goto CLEANUP;
          break;
        default:
          goto CLEANUP;
      }
    } else {
      if (http_buf_append_ch(&buf, (char)ch) != 0) goto CLEANUP;
    }
  }

  if (*p != '"') {
    goto CLEANUP;
  }

  if (http_buf_append_ch(&buf, '\0') != 0) {
    goto CLEANUP;
  }

  *out = buf.data;
  buf.data = NULL;
  *p_in = p + 1;
  exit_code = 0;

CLEANUP:
  http_buf_free(&buf);
  return exit_code;
}

static int http_parse_message_json(const char *body, size_t len, char **message_out) {
  char *copy = NULL;
  const char *p = NULL;
  char *key = NULL;
  char *value = NULL;
  int exit_code = 1;

  copy = (char *)malloc(len + 1u);
  if (copy == NULL) {
    return 1;
  }
  memcpy(copy, body, len);
  copy[len] = '\0';

  p = http_json_skip_ws(copy);
  if (*p != '{') goto CLEANUP;
  p = http_json_skip_ws(p + 1);
  if (http_json_parse_string(&p, &key) != 0) goto CLEANUP;
  p = http_json_skip_ws(p);
  if (*p != ':') goto CLEANUP;
  p = http_json_skip_ws(p + 1);
  if (http_json_parse_string(&p, &value) != 0) goto CLEANUP;
  p = http_json_skip_ws(p);
  if (*p != '}') goto CLEANUP;
  p = http_json_skip_ws(p + 1);
  if (*p != '\0') goto CLEANUP;
  if (strcmp(key, "message") != 0) goto CLEANUP;

  *message_out = value;
  value = NULL;
  exit_code = 0;

CLEANUP:
  if (key != NULL) free(key);
  if (value != NULL) free(value);
  if (copy != NULL) free(copy);
  return exit_code;
}

static int http_get_file_info(const char *path, uint64_t *size_out, uint64_t *mtime_out) {
#ifdef _WIN32
  struct _stat64 st;
  if (_stat64(path, &st) != 0) {
    return 1;
  }
#else
  struct stat st;
  if (stat(path, &st) != 0) {
    return 1;
  }
#endif

  if (st.st_size < 0) {
    return 1;
  }

  *size_out = (uint64_t)st.st_size;
  *mtime_out = (uint64_t)st.st_mtime;
  return 0;
}

static int http_file_entry_cmp(const void *lhs, const void *rhs) {
  const http_file_entry_t *a = (const http_file_entry_t *)lhs;
  const http_file_entry_t *b = (const http_file_entry_t *)rhs;
  if (a->mtime < b->mtime) return 1;
  if (a->mtime > b->mtime) return -1;
  return strcmp(a->name, b->name);
}

static void http_free_file_entries(http_file_entry_t *entries, size_t count) {
  if (entries == NULL) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(entries[i].name);
  }
  free(entries);
}

static int http_list_files(const char *dir, http_file_entry_t **entries_out,
                           size_t *count_out) {
  http_file_entry_t *entries = NULL;
  size_t count = 0;
  size_t cap = 0;
  int exit_code = 1;

#ifdef _WIN32
  char pattern[4096];
  WIN32_FIND_DATAA find_data;
  HANDLE handle = INVALID_HANDLE_VALUE;

  if (fs_join_path(pattern, sizeof(pattern), dir, "*") != 0) {
    return 1;
  }

  handle = FindFirstFileA(pattern, &find_data);
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND) {
      *entries_out = NULL;
      *count_out = 0;
      return 0;
    }
    return 1;
  }

  do {
    const char *name = find_data.cFileName;
    char full_path[4096];
    uint64_t size = 0;
    uint64_t mtime = 0;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      continue;
    }
    if (fs_join_path(full_path, sizeof(full_path), dir, name) != 0) {
      goto CLEANUP;
    }
    if (http_get_file_info(full_path, &size, &mtime) != 0) {
      continue;
    }

    if (count == cap) {
      size_t new_cap = cap == 0 ? 8u : cap * 2u;
      http_file_entry_t *new_entries =
        (http_file_entry_t *)realloc(entries, new_cap * sizeof(*entries));
      if (new_entries == NULL) {
        goto CLEANUP;
      }
      entries = new_entries;
      cap = new_cap;
    }

    entries[count].name = _strdup(name);
    if (entries[count].name == NULL) {
      goto CLEANUP;
    }
    entries[count].size = size;
    entries[count].mtime = mtime;
    count++;
  } while (FindNextFileA(handle, &find_data) != 0);

  if (GetLastError() != ERROR_NO_MORE_FILES) {
    goto CLEANUP;
  }

#else
  DIR *dp = opendir(dir);
  struct dirent *de = NULL;

  if (dp == NULL) {
    perror("opendir");
    return 1;
  }

  while ((de = readdir(dp)) != NULL) {
    const char *name = de->d_name;
    char full_path[4096];
    uint64_t size = 0;
    uint64_t mtime = 0;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }

    if (fs_join_path(full_path, sizeof(full_path), dir, name) != 0) {
      goto CLEANUP;
    }
    if (http_get_file_info(full_path, &size, &mtime) != 0) {
      continue;
    }

    if (count == cap) {
      size_t new_cap = cap == 0 ? 8u : cap * 2u;
      http_file_entry_t *new_entries =
        (http_file_entry_t *)realloc(entries, new_cap * sizeof(*entries));
      if (new_entries == NULL) {
        goto CLEANUP;
      }
      entries = new_entries;
      cap = new_cap;
    }

    entries[count].name = strdup(name);
    if (entries[count].name == NULL) {
      goto CLEANUP;
    }
    entries[count].size = size;
    entries[count].mtime = mtime;
    count++;
  }

#endif

  if (count > 1u) {
    qsort(entries, count, sizeof(*entries), http_file_entry_cmp);
  }

  *entries_out = entries;
  *count_out = count;
  exit_code = 0;
  entries = NULL;

CLEANUP:
#ifdef _WIN32
  if (handle != INVALID_HANDLE_VALUE) {
    FindClose(handle);
  }
#else
  if (dp != NULL) {
    closedir(dp);
  }
#endif
  http_free_file_entries(entries, count);
  return exit_code;
}

static int http_build_files_json(const char *dir, http_buf_t *out) {
  http_file_entry_t *entries = NULL;
  size_t count = 0;
  int exit_code = 1;

  if (http_list_files(dir, &entries, &count) != 0) {
    return 1;
  }

  if (http_buf_append_ch(out, '[') != 0) {
    goto CLEANUP;
  }

  for (size_t i = 0; i < count; i++) {
    char numbuf[64];
    if (i > 0 && http_buf_append_ch(out, ',') != 0) {
      goto CLEANUP;
    }
    if (http_buf_append_str(out, "{\"name\":\"") != 0 ||
        http_json_escape(out, entries[i].name) != 0 ||
        http_buf_append_str(out, "\",\"size\":") != 0) {
      goto CLEANUP;
    }

    int n = snprintf(numbuf, sizeof(numbuf), "%" PRIu64, entries[i].size);
    if (n < 0 || http_buf_append(out, numbuf, (size_t)n) != 0 ||
        http_buf_append_str(out, ",\"mtime\":") != 0) {
      goto CLEANUP;
    }

    n = snprintf(numbuf, sizeof(numbuf), "%" PRIu64, entries[i].mtime);
    if (n < 0 || http_buf_append(out, numbuf, (size_t)n) != 0 ||
        http_buf_append_ch(out, '}') != 0) {
      goto CLEANUP;
    }
  }

  if (http_buf_append_ch(out, ']') != 0) {
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  http_free_file_entries(entries, count);
  return exit_code;
}

static int http_send_file(socket_t conn, const server_opt_t *ser_opt,
                          const char *file_name) {
  char path[4096];
  char header[1024];
  int fd = -1;
  uint64_t size = 0;
  uint64_t mtime = 0;
  int exit_code = 1;
  char safe_name[512];
  size_t safe_len = 0;

  (void)mtime;

  if (fs_validate_file_name(file_name) != 0) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid file name");
  }
  if (fs_join_path(path, sizeof(path), ser_opt->path, file_name) != 0) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid path");
  }
  if (http_get_file_info(path, &size, &mtime) != 0) {
    return http_send_json_error(conn, 404, "Not Found", "file not found");
  }

#ifdef _WIN32
  fd = fs_open(path, O_RDONLY | O_BINARY, 0);
#else
  fd = fs_open(path, O_RDONLY, 0);
#endif
  if (fd == -1) {
    perror("open(http_download)");
    return http_send_json_error(conn, 404, "Not Found", "file not found");
  }

  for (const char *p = file_name; *p != '\0' && safe_len + 1u < sizeof(safe_name); p++) {
    if (*p == '"' || *p == '\\' || *p == '\r' || *p == '\n') {
      safe_name[safe_len++] = '_';
    } else {
      safe_name[safe_len++] = *p;
    }
  }
  safe_name[safe_len] = '\0';

  int n = snprintf(header, sizeof(header),
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: %" PRIu64 "\r\n"
                   "Content-Disposition: attachment; filename=\"%s\"\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   size, safe_name);
  if (n < 0 || (size_t)n >= sizeof(header)) {
    goto CLEANUP;
  }

  if (send_all(conn, header, (size_t)n) != (ssize_t)n) {
    goto CLEANUP;
  }

  net_send_file_result_t send_file_res =
    net_send_file_best_effort(conn, fd, size);
  if (send_file_res != NET_SEND_FILE_OK) {
    if (send_file_res == NET_SEND_FILE_SOURCE_CHANGED) {
      fprintf(stderr, "source file changed during http download\n");
    } else {
      sock_perror("sendfile(http_download)");
    }
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  if (fd != -1) fs_close(fd);
  return exit_code;
}

static int http_handle_webui_asset(socket_t conn, const webui_asset_t *asset) {
  if (asset == NULL) {
    return 1;
  }

  return http_send_response(conn, 200, "OK", asset->content_type,
                            asset->body, asset->body_len, NULL);
}

static int http_handle_files_list(socket_t conn, const server_opt_t *ser_opt) {
  http_buf_t body = {0};
  int exit_code = 1;

  if (http_build_files_json(ser_opt->path, &body) != 0) {
    return http_send_json_error(conn, 500, "Internal Server Error",
                                "failed to list files");
  }

  if (http_send_response(conn, 200, "OK", "application/json; charset=utf-8",
                         body.data, body.len, NULL) != 0) {
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  http_buf_free(&body);
  return exit_code;
}

static int http_handle_messages_post(socket_t conn, const server_opt_t *ser_opt,
                                     const http_request_t *req) {
  char *body = NULL;
  char *message = NULL;
  http_buf_t response = {0};
  int exit_code = 1;

  if (!req->has_content_length) {
    return http_send_json_error(conn, 411, "Length Required", "content-length required");
  }
  if (req->content_length > HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE) {
    return http_send_json_error(conn, 413, "Payload Too Large", "message too large");
  }
  if (!http_ascii_starts_with(req->content_type, "application/json")) {
    return http_send_json_error(conn, 415, "Unsupported Media Type",
                                "content-type must be application/json");
  }

  size_t body_len = (size_t)req->content_length;
  body = (char *)malloc(body_len + 1u);
  if (body == NULL) {
    return http_send_json_error(conn, 500, "Internal Server Error", "allocation failed");
  }
  
  recv_all(conn, body, body_len);
  body[body_len] = '\0';

  if (http_parse_message_json(body, (size_t)req->content_length, &message) != 0) {
    (void)http_send_json_error(conn, 400, "Bad Request", "invalid message payload");
    goto CLEANUP;
  }
  (void)ser_opt;
  if (message_store_set(message) != 0) {
    (void)http_send_json_error(conn, 500, "Internal Server Error",
                               "failed to store message");
    goto CLEANUP;
  }

  if (http_buf_append_str(&response, "{\"ok\":true}") != 0) {
    goto CLEANUP;
  }

  if (http_send_response(conn, 201, "Created", "application/json; charset=utf-8",
                         response.data, response.len, NULL) != 0) {
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  if (body != NULL) free(body);
  if (message != NULL) free(message);
  http_buf_free(&response);
  return exit_code;
}

static int http_handle_messages_latest_get(socket_t conn) {
  http_buf_t response = {0};
  char *message = NULL;
  int has_message = 0;
  int exit_code = 1;

  if (message_store_get_copy(&message, &has_message) != 0) {
    return http_send_json_error(conn, 500, "Internal Server Error",
                                "failed to load message");
  }

  if (http_buf_append_str(&response, "{\"has_message\":") != 0) {
    goto CLEANUP;
  }
  if (http_buf_append_str(&response, has_message ? "true" : "false") != 0) {
    goto CLEANUP;
  }
  if (http_buf_append_str(&response, ",\"message\":\"") != 0) {
    goto CLEANUP;
  }
  if (has_message && http_json_escape(&response, message) != 0) {
    goto CLEANUP;
  }
  if (http_buf_append_str(&response, "\"}") != 0) {
    goto CLEANUP;
  }

  if (http_send_response(conn, 200, "OK", "application/json; charset=utf-8",
                         response.data, response.len, NULL) != 0) {
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  if (message != NULL) free(message);
  http_buf_free(&response);
  return exit_code;
}

static int http_handle_messages_stream(socket_t conn) {
  uint64_t version = 0;
  char *message = NULL;
  int has_message = 0;
  int exit_code = 1;

  if (http_send_sse_headers(conn) != 0) {
    return 1;
  }

  if (message_store_get_snapshot(&message, &has_message, &version) != 0) {
    return 1;
  }
  if (has_message && http_send_sse_message_event(conn, message) != 0) {
    goto CLEANUP;
  }
  free(message);
  message = NULL;

  for (;;) {
    if (message_store_wait_for_update(version, 15000u, &message, &has_message,
                                      &version) != 0) {
      goto CLEANUP;
    }

    if (shutdown_requested()) {
      exit_code = 0;
      goto CLEANUP;
    }

    if (message == NULL && !has_message) {
      if (http_send_sse_keepalive(conn) != 0) {
        goto CLEANUP;
      }
      continue;
    }

    if (http_send_sse_message_event(conn, message) != 0) {
      goto CLEANUP;
    }
    free(message);
    message = NULL;
  }

  exit_code = 0;

CLEANUP:
  if (message != NULL) {
    free(message);
  }
  return exit_code;
}

static int http_handle_file_put(socket_t conn, const server_opt_t *ser_opt,
                                const http_request_t *req, const char *file_name) {
  http_buf_t response = {0};
  uint64_t size = 0;
  uint64_t mtime = 0;
  int exit_code = 1;
  char numbuf[64];

  if (!req->has_content_length) {
    return http_send_json_error(conn, 411, "Length Required", "content-length required");
  }
  if (req->content_length > HF_HTTP_UPLOAD_MAX) {
    return http_send_json_error(conn, 413, "Payload Too Large", "upload too large");
  }
  if (!http_ascii_starts_with(req->content_type, "application/octet-stream")) {
    (void)http_discard_body(conn, req->content_length);
    return http_send_json_error(conn, 415, "Unsupported Media Type",
                                "content-type must be application/octet-stream");
  }
  if (fs_validate_file_name(file_name) != 0) {
    (void)http_discard_body(conn, req->content_length);
    return http_send_json_error(conn, 400, "Bad Request", "invalid file name");
  }

  char saved_path[4096];
  if (transfer_recv_socket_file(conn, ser_opt->path, file_name,
                                req->content_length, "recv(http_body)",
                                "http upload ended early", saved_path,
                                sizeof(saved_path)) != 0) {
    return http_send_json_error(conn, 500, "Internal Server Error", "failed to save file");
  }

  if (http_get_file_info(saved_path, &size, &mtime) != 0) {
    return http_send_json_error(conn, 500, "Internal Server Error", "saved file missing");
  }

  if (http_buf_append_str(&response, "{\"name\":\"") != 0 ||
      http_json_escape(&response, file_name) != 0 ||
      http_buf_append_str(&response, "\",\"size\":") != 0) {
    goto CLEANUP;
  }
  int n = snprintf(numbuf, sizeof(numbuf), "%" PRIu64, size);
  if (n < 0 || http_buf_append(&response, numbuf, (size_t)n) != 0 ||
      http_buf_append_str(&response, ",\"mtime\":") != 0) {
    goto CLEANUP;
  }
  n = snprintf(numbuf, sizeof(numbuf), "%" PRIu64, mtime);
  if (n < 0 || http_buf_append(&response, numbuf, (size_t)n) != 0 ||
      http_buf_append_ch(&response, '}') != 0) {
    goto CLEANUP;
  }

  if (http_send_response(conn, 201, "Created", "application/json; charset=utf-8",
                         response.data, response.len, NULL) != 0) {
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  http_buf_free(&response);
  return exit_code;
}

static int http_handle_file_delete(socket_t conn, const server_opt_t *ser_opt,
                                   const char *file_name) {
  http_buf_t response = {0};
  char path[4096];
  int exit_code = 1;

  if (fs_validate_file_name(file_name) != 0) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid file name");
  }
  if (fs_join_path(path, sizeof(path), ser_opt->path, file_name) != 0) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid path");
  }
  if (remove(path) != 0) {
    if (errno == ENOENT) {
      return http_send_json_error(conn, 404, "Not Found", "file not found");
    }
    perror("remove(http_delete)");
    return http_send_json_error(conn, 500, "Internal Server Error", "failed to delete file");
  }

  if (http_buf_append_str(&response, "{\"ok\":true}") != 0) {
    goto CLEANUP;
  }
  if (http_send_response(conn, 200, "OK", "application/json; charset=utf-8",
                         response.data, response.len, NULL) != 0) {
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  http_buf_free(&response);
  return exit_code;
}

int http_handle_connection(socket_t conn, const server_opt_t *ser_opt) {
  char header_block[HF_HTTP_HEADER_MAX];
  char route_name[HF_PROTOCOL_MAX_FILE_NAME_LEN + 1u];
  http_request_t req = {0};
  const webui_asset_t *asset = NULL;
  int read_res = http_read_header_block(conn, header_block, sizeof(header_block));
  int parse_res = 0;

  if (read_res == -1) {
    sock_perror("recv(http_header)");
    return 1;
  }
  if (read_res == 1) {
    return 1;
  }
  if (read_res == 2) {
    return http_send_json_error(conn, 431, "Request Header Fields Too Large",
                                "header too large");
  }

  parse_res = http_parse_request(header_block, &req);
  if (parse_res == 1) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid request");
  }
  if (parse_res == 2) {
    return http_send_json_error(conn, 505, "HTTP Version Not Supported",
                                "only HTTP/1.1 is supported");
  }

  if (strcmp(req.method, "GET") == 0) {
    asset = webui_find_asset(req.path);
    if (asset != NULL) {
      return http_handle_webui_asset(conn, asset);
    }
  }
  if (strcmp(req.path, "/api/files") == 0) {
    if (strcmp(req.method, "GET") != 0) {
      return http_send_json_error(conn, 405, "Method Not Allowed", "method not allowed");
    }
    return http_handle_files_list(conn, ser_opt);
  }
  if (strcmp(req.path, "/api/messages") == 0) {
    if (strcmp(req.method, "POST") == 0) {
      return http_handle_messages_post(conn, ser_opt, &req);
    }
    return http_send_json_error(conn, 405, "Method Not Allowed", "method not allowed");
  }
  if (strcmp(req.path, "/api/messages/latest") == 0) {
    if (strcmp(req.method, "GET") == 0) {
      return http_handle_messages_latest_get(conn);
    }
    return http_send_json_error(conn, 405, "Method Not Allowed", "method not allowed");
  }
  if (strcmp(req.path, "/api/messages/stream") == 0) {
    if (strcmp(req.method, "GET") == 0) {
      return http_handle_messages_stream(conn);
    }
    return http_send_json_error(conn, 405, "Method Not Allowed", "method not allowed");
  }
  if (strncmp(req.path, "/api/files/", 11) == 0) {
    if (http_decode_name(req.path + 11, route_name, sizeof(route_name)) != 0) {
      return http_send_json_error(conn, 400, "Bad Request", "invalid file name");
    }
    if (strcmp(req.method, "GET") == 0) {
      return http_send_file(conn, ser_opt, route_name);
    }
    if (strcmp(req.method, "PUT") == 0) {
      return http_handle_file_put(conn, ser_opt, &req, route_name);
    }
    if (strcmp(req.method, "DELETE") == 0) {
      return http_handle_file_delete(conn, ser_opt, route_name);
    }
    return http_send_json_error(conn, 405, "Method Not Allowed", "method not allowed");
  }

  return http_send_json_error(conn, 404, "Not Found", "route not found");
}

#ifdef _WIN32
static unsigned __stdcall http_connection_thread_main(void *arg) {
#else
static void *http_connection_thread_main(void *arg) {
#endif
  http_conn_ctx_t *ctx = (http_conn_ctx_t *)arg;
  socket_t conn = ctx->conn;
  server_opt_t opt = ctx->opt;
  http_conn_entry_t *entry = ctx->entry;

  free(ctx);
  (void)http_handle_connection(conn, &opt);
  socket_close(conn);
  http_conn_tracker_end(entry);

#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

static int http_start_connection_thread(socket_t conn, const server_opt_t *ser_opt) {
  http_conn_ctx_t *ctx = (http_conn_ctx_t *)malloc(sizeof(*ctx));
  http_conn_entry_t *entry = NULL;
  if (ctx == NULL) {
    perror("malloc(http_conn_ctx)");
    return 1;
  }

  entry = http_conn_tracker_begin(conn);
  if (entry == NULL) {
    free(ctx);
    return 1;
  }

  ctx->conn = conn;
  ctx->opt = *ser_opt;
  ctx->entry = entry;

#ifdef _WIN32
  uintptr_t handle =
    _beginthreadex(NULL, 0, http_connection_thread_main, ctx, 0, NULL);
  if (handle == 0) {
    fprintf(stderr, "_beginthreadex(http_conn) failed\n");
    http_conn_tracker_end(entry);
    free(ctx);
    return 1;
  }
  CloseHandle((HANDLE)handle);
#else
  pthread_t tid;
  int err = pthread_create(&tid, NULL, http_connection_thread_main, ctx);
  if (err != 0) {
    fprintf(stderr, "pthread_create(http_conn): %s\n", strerror(err));
    http_conn_tracker_end(entry);
    free(ctx);
    return 1;
  }
  (void)pthread_detach(tid);
#endif

  return 0;
}

int http_server(socket_t listener, const server_opt_t *ser_opt) {
  int exit_code = 0;

  if (http_conn_tracker_init() != 0) {
    fprintf(stderr, "failed to initialize http connection tracker\n");
    return 1;
  }

  for (;;) {
    int ready = 0;
    if (shutdown_requested()) {
      exit_code = shutdown_exit_code();
      break;
    }

    if (net_wait_readable(listener, 250u, &ready) != 0) {
      if (shutdown_requested()) {
        exit_code = shutdown_exit_code();
        break;
      }
      sock_perror("select(accept(http))");
      exit_code = 1;
      continue;
    }
    if (!ready) {
      continue;
    }

    socket_t conn = accept(listener, NULL, NULL);
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
      sock_perror("accept(http)");
      exit_code = 1;
      continue;
    }

    if (http_start_connection_thread(conn, ser_opt) != 0) {
      socket_close(conn);
      exit_code = 1;
    }
  }

  http_conn_tracker_shutdown();
  http_conn_tracker_wait_idle();
  http_conn_tracker_cleanup();

  return exit_code;
}
