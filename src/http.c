#include "app_service.h"
#include "http.h"

#include "fs.h"
#include "message_store.h"
#include "net.h"
#include "protocol.h"
#include "shutdown.h"
#include "webui.h"
#include "picohttpparser.h"

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
#define HF_HTTP_MAX_HEADERS 64u
#define HF_HTTP_PATH_MAX 1024u
#define HF_HTTP_CONTENT_TYPE_MAX 128u
#define HF_HTTP_UPLOAD_MAX (16ULL * 1024ULL * 1024ULL * 1024ULL)
#define HF_HTTP_MESSAGE_BODY_TIMEOUT_MS 30000u
#define HF_HTTP_UPLOAD_BODY_TIMEOUT_MS 120000u

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} http_buf_t;

typedef struct {
  char method[8];
  char path[HF_HTTP_PATH_MAX];
  char query[HF_HTTP_PATH_MAX];
  char content_type[HF_HTTP_CONTENT_TYPE_MAX];
  uint64_t content_length;
  int has_content_length;
  int has_transfer_encoding;
} http_request_t;

typedef struct {
  char *name;
  char *path;
  fs_path_kind_t kind;
  uint64_t size;
  uint64_t mtime;
} http_file_entry_t;

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

static int http_set_connection_recv_timeout(socket_t conn, uint32_t timeout_ms) {
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

static int http_header_name_equals(const struct phr_header *header,
                                   const char *name) {
  size_t i = 0;

  if (header == NULL || name == NULL) {
    return 0;
  }

  while (name[i] != '\0') {
    if (i >= header->name_len) {
      return 0;
    }
    if (tolower((unsigned char)header->name[i]) !=
        tolower((unsigned char)name[i])) {
      return 0;
    }
    i++;
  }

  return i == header->name_len;
}

static int http_copy_header_value(char *dst, size_t dst_cap,
                                  const char *src, size_t src_len) {
  if (dst == NULL || src == NULL || dst_cap == 0u || src_len >= dst_cap) {
    return 1;
  }

  memcpy(dst, src, src_len);
  dst[src_len] = '\0';
  return 0;
}

static int http_parse_request(char *header_block, http_request_t *req) {
  const char *method = NULL;
  const char *path = NULL;
  size_t method_len = 0;
  size_t path_len = 0;
  int minor_version = -1;
  struct phr_header headers[HF_HTTP_MAX_HEADERS];
  size_t num_headers = HF_HTTP_MAX_HEADERS;
  int parse_res = 0;
  char header_value[HF_HTTP_CONTENT_TYPE_MAX];

  if (header_block == NULL || req == NULL) {
    return 1;
  }

  memset(req, 0, sizeof(*req));

  parse_res = phr_parse_request(header_block, strlen(header_block),
                                &method, &method_len,
                                &path, &path_len,
                                &minor_version, headers, &num_headers, 0);
  if (parse_res <= 0) {
    return 1;
  }

  if (minor_version != 1) {
    return 2;
  }

  if (method_len >= sizeof(req->method) || path_len >= sizeof(req->path)) {
    return 1;
  }

  memcpy(req->method, method, method_len);
  req->method[method_len] = '\0';
  memcpy(req->path, path, path_len);
  req->path[path_len] = '\0';
  req->query[0] = '\0';

  for (size_t i = 0; i < num_headers; i++) {
    const struct phr_header *header = &headers[i];

    if (header->name == NULL) {
      return 1;
    }
    if (http_header_name_equals(header, "Content-Length")) {
      if (http_copy_header_value(header_value, sizeof(header_value),
                                 header->value, header->value_len) != 0) {
        return 1;
      }
      if (http_parse_u64(header_value, &req->content_length) != 0) {
        return 1;
      }
      req->has_content_length = 1;
    } else if (http_header_name_equals(header, "Content-Type")) {
      if (http_copy_header_value(req->content_type, sizeof(req->content_type),
                                 header->value, header->value_len) != 0) {
        return 1;
      }
    } else if (http_header_name_equals(header, "Transfer-Encoding")) {
      req->has_transfer_encoding = 1;
    }
  }

  char *query = strchr(req->path, '?');
  if (query != NULL) {
    if (strlen(query + 1) >= sizeof(req->query)) {
      return 1;
    }
    memcpy(req->query, query + 1, strlen(query + 1) + 1u);
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

static int http_query_get_value(const char *query, const char *key,
                                char *out, size_t out_cap) {
  size_t key_len = 0;

  if (key == NULL || out == NULL || out_cap == 0u) {
    return 1;
  }

  out[0] = '\0';
  if (query == NULL || query[0] == '\0') {
    return 1;
  }

  key_len = strlen(key);
  while (*query != '\0') {
    const char *amp = strchr(query, '&');
    const char *entry_end = amp != NULL ? amp : query + strlen(query);
    const char *eq = memchr(query, '=', (size_t)(entry_end - query));
    size_t name_len = eq != NULL ? (size_t)(eq - query) : (size_t)(entry_end - query);
    const char *value = eq != NULL ? eq + 1 : entry_end;
    size_t value_len = (size_t)(entry_end - value);

    if (name_len == key_len && memcmp(query, key, key_len) == 0) {
      if (value_len >= out_cap) {
        return 2;
      }
      memcpy(out, value, value_len);
      out[value_len] = '\0';
      return 0;
    }

    if (amp == NULL) {
      break;
    }
    query = amp + 1;
  }

  return 1;
}

static int http_build_relative_child_path(char *out, size_t out_cap,
                                          const char *base, const char *name) {
  int n = 0;

  if (out == NULL || out_cap == 0u || name == NULL) {
    return 1;
  }
  if (base == NULL || base[0] == '\0') {
    n = snprintf(out, out_cap, "%s", name);
  } else {
    n = snprintf(out, out_cap, "%s/%s", base, name);
  }
  return n < 0 || (size_t)n >= out_cap ? 1 : 0;
}

static const char *http_path_kind_name(fs_path_kind_t kind) {
  switch (kind) {
    case FS_PATH_KIND_FILE:
      return "file";
    case FS_PATH_KIND_DIR:
      return "dir";
    case FS_PATH_KIND_SYMLINK:
      return "symlink";
    default:
      return "other";
  }
}

static const char *http_relative_basename(const char *path) {
  const char *base = NULL;

  if (path == NULL) {
    return NULL;
  }
  base = strrchr(path, '/');
  return base != NULL ? base + 1 : path;
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

static int http_json_parse_hex4(const char *p, uint16_t *out) {
  uint16_t value = 0;

  if (p == NULL || out == NULL) {
    return 1;
  }

  for (size_t i = 0; i < 4u; i++) {
    unsigned char ch = (unsigned char)p[i];
    uint16_t digit = 0;
    if (!isxdigit(ch)) {
      return 1;
    }
    if (isdigit(ch)) {
      digit = (uint16_t)(ch - '0');
    } else {
      digit = (uint16_t)(10 + tolower(ch) - 'a');
    }
    value = (uint16_t)(value * 16u + digit);
  }

  *out = value;
  return 0;
}

static int http_buf_append_utf8(http_buf_t *buf, uint32_t cp) {
  char encoded[4];
  size_t len = 0;

  if (cp <= 0x7Fu) {
    encoded[0] = (char)cp;
    len = 1u;
  } else if (cp <= 0x7FFu) {
    encoded[0] = (char)(0xC0u | (cp >> 6));
    encoded[1] = (char)(0x80u | (cp & 0x3Fu));
    len = 2u;
  } else if (cp <= 0xFFFFu) {
    encoded[0] = (char)(0xE0u | (cp >> 12));
    encoded[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    encoded[2] = (char)(0x80u | (cp & 0x3Fu));
    len = 3u;
  } else if (cp <= 0x10FFFFu) {
    encoded[0] = (char)(0xF0u | (cp >> 18));
    encoded[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    encoded[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    encoded[3] = (char)(0x80u | (cp & 0x3Fu));
    len = 4u;
  } else {
    return 1;
  }

  return http_buf_append(buf, encoded, len);
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
        case 'u': {
          uint16_t code_unit = 0;
          uint32_t code_point = 0;
          if (http_json_parse_hex4(p, &code_unit) != 0) goto CLEANUP;
          p += 4;
          if (code_unit >= 0xD800u && code_unit <= 0xDBFFu) {
            uint16_t low_surrogate = 0;
            if (p[0] != '\\' || p[1] != 'u' ||
                http_json_parse_hex4(p + 2, &low_surrogate) != 0 ||
                low_surrogate < 0xDC00u || low_surrogate > 0xDFFFu) {
              goto CLEANUP;
            }
            code_point = 0x10000u +
                         (((uint32_t)(code_unit - 0xD800u) << 10u) |
                          (uint32_t)(low_surrogate - 0xDC00u));
            p += 6;
          } else {
            if (code_unit >= 0xDC00u && code_unit <= 0xDFFFu) {
              goto CLEANUP;
            }
            code_point = (uint32_t)code_unit;
          }
          if (http_buf_append_utf8(&buf, code_point) != 0) goto CLEANUP;
          break;
        }
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
    free(entries[i].path);
  }
  free(entries);
}

static int http_list_files(const char *dir, const char *relative_dir,
                           http_file_entry_t **entries_out,
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
    char relative_path[HF_HTTP_PATH_MAX];
    fs_path_info_t info = {0};

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    if (fs_join_path(full_path, sizeof(full_path), dir, name) != 0) {
      goto CLEANUP;
    }
    if (http_build_relative_child_path(relative_path, sizeof(relative_path),
                                       relative_dir, name) != 0) {
      goto CLEANUP;
    }
    if (fs_get_path_info(full_path, &info) != 0) {
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
    entries[count].path = _strdup(relative_path);
    if (entries[count].path == NULL) {
      free(entries[count].name);
      entries[count].name = NULL;
      goto CLEANUP;
    }
    entries[count].kind = info.kind;
    entries[count].size = info.kind == FS_PATH_KIND_FILE ? info.size : 0;
    entries[count].mtime = info.mtime;
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
    char relative_path[HF_HTTP_PATH_MAX];
    fs_path_info_t info = {0};

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }

    if (fs_join_path(full_path, sizeof(full_path), dir, name) != 0) {
      goto CLEANUP;
    }
    if (http_build_relative_child_path(relative_path, sizeof(relative_path),
                                       relative_dir, name) != 0) {
      goto CLEANUP;
    }
    if (fs_get_path_info(full_path, &info) != 0) {
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
    entries[count].path = strdup(relative_path);
    if (entries[count].path == NULL) {
      free(entries[count].name);
      entries[count].name = NULL;
      goto CLEANUP;
    }
    entries[count].kind = info.kind;
    entries[count].size = info.kind == FS_PATH_KIND_FILE ? info.size : 0;
    entries[count].mtime = info.mtime;
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

static int http_build_files_json(const char *dir, const char *relative_dir,
                                 http_buf_t *out) {
  http_file_entry_t *entries = NULL;
  size_t count = 0;
  int exit_code = 1;

  if (http_list_files(dir, relative_dir, &entries, &count) != 0) {
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
        http_buf_append_str(out, "\",\"path\":\"") != 0 ||
        http_json_escape(out, entries[i].path) != 0 ||
        http_buf_append_str(out, "\",\"kind\":\"") != 0 ||
        http_json_escape(out, http_path_kind_name(entries[i].kind)) != 0 ||
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
                          const char *relative_path) {
  char header[1024];
  app_download_t download = {.fd = -1};
  int exit_code = 1;
  char safe_name[512];
  size_t safe_len = 0;

  if (fs_validate_relative_path(relative_path) != 0) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid file path");
  }
  if (app_prepare_download(ser_opt->path, relative_path, &download) != PROTOCOL_OK) {
    return http_send_json_error(conn, 404, "Not Found", "file not found");
  }

  for (const char *p = http_relative_basename(relative_path);
       *p != '\0' && safe_len + 1u < sizeof(safe_name); p++) {
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
                   download.info.size, safe_name);
  if (n < 0 || (size_t)n >= sizeof(header)) {
    goto CLEANUP;
  }

  if (send_all(conn, header, (size_t)n) != (ssize_t)n) {
    goto CLEANUP;
  }

  net_send_file_result_t send_file_res =
    net_send_file_best_effort(conn, download.fd, download.info.size);
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
  app_download_cleanup(&download);
  return exit_code;
}

static int http_handle_webui_asset(socket_t conn, const webui_asset_t *asset) {
  if (asset == NULL) {
    return 1;
  }

  return http_send_response(conn, 200, "OK", asset->content_type,
                            asset->body, asset->body_len, NULL);
}

static int http_handle_files_list(socket_t conn, const server_opt_t *ser_opt,
                                  const http_request_t *req) {
  http_buf_t body = {0};
  char encoded_path[HF_HTTP_PATH_MAX];
  char relative_dir[HF_HTTP_PATH_MAX];
  char dir_path[4096];
  fs_path_info_t info = {0};
  int exit_code = 1;

  relative_dir[0] = '\0';
  if (http_query_get_value(req->query, "path", encoded_path,
                           sizeof(encoded_path)) == 0) {
    if (http_decode_name(encoded_path, relative_dir, sizeof(relative_dir)) != 0) {
      return http_send_json_error(conn, 400, "Bad Request", "invalid path");
    }
    if (relative_dir[0] != '\0' && fs_validate_relative_path(relative_dir) != 0) {
      return http_send_json_error(conn, 400, "Bad Request", "invalid path");
    }
  }

  if (fs_join_relative_path(dir_path, sizeof(dir_path), ser_opt->path, relative_dir) != 0) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid path");
  }
  if (fs_get_path_info(dir_path, &info) != 0) {
    return http_send_json_error(conn, 404, "Not Found", "path not found");
  }
  if (info.kind != FS_PATH_KIND_DIR) {
#ifndef _WIN32
    if (relative_dir[0] == '\0' && info.kind == FS_PATH_KIND_SYMLINK) {
      DIR *dp = opendir(dir_path);
      if (dp == NULL) {
        return http_send_json_error(conn, 400, "Bad Request",
                                    "path is not a directory");
      }
      closedir(dp);
    } else {
      return http_send_json_error(conn, 400, "Bad Request", "path is not a directory");
    }
#else
    return http_send_json_error(conn, 400, "Bad Request", "path is not a directory");
#endif
  }

  if (http_build_files_json(dir_path, relative_dir, &body) != 0) {
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
  const char *body_data = NULL;
  http_buf_t response = {0};
  int exit_code = 1;
  ssize_t n = 0;
  size_t body_len = 0;

  if (req->has_transfer_encoding) {
    return http_send_json_error(conn, 501, "Not Implemented",
                                "transfer-encoding not supported");
  }

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

  if (http_set_connection_recv_timeout(conn, HF_HTTP_MESSAGE_BODY_TIMEOUT_MS) != 0) {
    sock_perror("setsockopt(SO_RCVTIMEO)");
  }

  body_len = (size_t)req->content_length;
  body = (char *)malloc(body_len + 1u);
  if (body == NULL) {
    return http_send_json_error(conn, 500, "Internal Server Error", "allocation failed");
  }

  n = recv_all(conn, body, body_len);
  if (n < 0) {
    sock_perror("recv_all(http_message)");
    goto CLEANUP;
  }
  if ((size_t)n != body_len) {
    fprintf(stderr, "http error: unexpected EOF while receiving message body\n");
    goto CLEANUP;
  }
  body[body_len] = '\0';
  body_data = body;

  if (http_parse_message_json(body_data, body_len, &message) != 0) {
    (void)http_send_json_error(conn, 400, "Bad Request", "invalid message payload");
    goto CLEANUP;
  }
  (void)ser_opt;
  if (app_submit_message(message) != PROTOCOL_OK) {
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
                                const http_request_t *req, const char *relative_path) {
  http_buf_t response = {0};
  fs_path_info_t info = {0};
  int exit_code = 1;
  char numbuf[64];
  protocol_result_t recv_result = PROTOCOL_ERR_IO;

  if (req->has_transfer_encoding) {
    return http_send_json_error(conn, 501, "Not Implemented",
                                "transfer-encoding not supported");
  }

  if (!req->has_content_length) {
    return http_send_json_error(conn, 411, "Length Required", "content-length required");
  }
  if (req->content_length > HF_HTTP_UPLOAD_MAX) {
    return http_send_json_error(conn, 413, "Payload Too Large", "upload too large");
  }
  if (!http_ascii_starts_with(req->content_type, "application/octet-stream")) {
    if (req->has_content_length) {
      (void)http_discard_body(conn, req->content_length);
    }
    return http_send_json_error(conn, 415, "Unsupported Media Type",
                                "content-type must be application/octet-stream");
  }
  if (fs_validate_relative_path(relative_path) != 0) {
    if (req->has_content_length) {
      (void)http_discard_body(conn, req->content_length);
    }
    return http_send_json_error(conn, 400, "Bad Request", "invalid file path");
  }

  if (http_set_connection_recv_timeout(conn, HF_HTTP_UPLOAD_BODY_TIMEOUT_MS) != 0) {
    sock_perror("setsockopt(SO_RCVTIMEO)");
  }

  char saved_path[4096];
  recv_result = app_receive_file(conn, ser_opt->path, relative_path,
                                 req->content_length, APP_UPLOAD_HTTP,
                                 saved_path, sizeof(saved_path));
  if (recv_result == PROTOCOL_ERR_MSG_TOO_LARGE) {
    return http_send_json_error(conn, 413, "Payload Too Large", "upload too large");
  }
  if (recv_result == PROTOCOL_ERR_INVALID_ARGUMENT) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid file path");
  }
  if (recv_result != PROTOCOL_OK) {
    return http_send_json_error(conn, 500, "Internal Server Error", "failed to save file");
  }

  if (fs_get_path_info(saved_path, &info) != 0 || info.kind != FS_PATH_KIND_FILE) {
    return http_send_json_error(conn, 500, "Internal Server Error", "saved file missing");
  }

  if (http_buf_append_str(&response, "{\"name\":\"") != 0 ||
      http_json_escape(&response, http_relative_basename(relative_path)) != 0 ||
      http_buf_append_str(&response, "\",\"path\":\"") != 0 ||
      http_json_escape(&response, relative_path) != 0 ||
      http_buf_append_str(&response, "\",\"kind\":\"file\",\"size\":") != 0) {
    goto CLEANUP;
  }
  int n = snprintf(numbuf, sizeof(numbuf), "%" PRIu64, info.size);
  if (n < 0 || http_buf_append(&response, numbuf, (size_t)n) != 0 ||
      http_buf_append_str(&response, ",\"mtime\":") != 0) {
    goto CLEANUP;
  }
  n = snprintf(numbuf, sizeof(numbuf), "%" PRIu64, info.mtime);
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
                                   const char *relative_path) {
  http_buf_t response = {0};
  char path[4096];
  fs_path_info_t info = {0};
  int exit_code = 1;

  if (fs_validate_relative_path(relative_path) != 0) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid file path");
  }
  if (fs_join_relative_path(path, sizeof(path), ser_opt->path, relative_path) != 0) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid path");
  }
  if (fs_get_path_info(path, &info) != 0) {
    return http_send_json_error(conn, 404, "Not Found", "path not found");
  }
  if (info.kind == FS_PATH_KIND_FILE) {
    if (remove(path) != 0) {
      if (errno == ENOENT) {
        return http_send_json_error(conn, 404, "Not Found", "path not found");
      }
      perror("remove(http_delete)");
      return http_send_json_error(conn, 500, "Internal Server Error", "failed to delete path");
    }
  } else {
    if (fs_remove_tree(path) != 0) {
      if (errno == ENOENT) {
        return http_send_json_error(conn, 404, "Not Found", "path not found");
      }
      perror("remove_tree(http_delete)");
      return http_send_json_error(conn, 500, "Internal Server Error", "failed to delete path");
    }
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

typedef int (*http_exact_route_handler_t)(socket_t conn,
                                          const server_opt_t *ser_opt,
                                          const http_request_t *req);

typedef struct {
  const char *path;
  const char *method;
  http_exact_route_handler_t handler;
} http_exact_route_t;

static int http_route_files_list(socket_t conn, const server_opt_t *ser_opt,
                                 const http_request_t *req) {
  return http_handle_files_list(conn, ser_opt, req);
}

static int http_route_messages_post(socket_t conn, const server_opt_t *ser_opt,
                                    const http_request_t *req) {
  return http_handle_messages_post(conn, ser_opt, req);
}

static int http_route_messages_latest_get(socket_t conn,
                                          const server_opt_t *ser_opt,
                                          const http_request_t *req) {
  (void)ser_opt;
  (void)req;
  return http_handle_messages_latest_get(conn);
}

static int http_route_messages_stream(socket_t conn,
                                      const server_opt_t *ser_opt,
                                      const http_request_t *req) {
  (void)ser_opt;
  (void)req;
  return http_handle_messages_stream(conn);
}

static const http_exact_route_t http_exact_routes[] = {
  {"/api/files", "GET", http_route_files_list},
  {"/api/messages", "POST", http_route_messages_post},
  {"/api/messages/latest", "GET", http_route_messages_latest_get},
  {"/api/messages/stream", "GET", http_route_messages_stream},
};

static int http_dispatch_exact_route(socket_t conn,
                                     const server_opt_t *ser_opt,
                                     const http_request_t *req) {
  size_t route_count = sizeof(http_exact_routes) / sizeof(http_exact_routes[0]);

  for (size_t i = 0; i < route_count; i++) {
    const http_exact_route_t *route = &http_exact_routes[i];

    if (strcmp(req->path, route->path) != 0) {
      continue;
    }
    if (strcmp(req->method, route->method) != 0) {
      return http_send_json_error(conn, 405, "Method Not Allowed", "method not allowed");
    }
    return route->handler(conn, ser_opt, req);
  }

  return -1;
}

static int http_dispatch_file_route(socket_t conn,
                                    const server_opt_t *ser_opt,
                                    const http_request_t *req) {
  char route_name[HF_HTTP_PATH_MAX];

  if (strncmp(req->path, "/api/files/", 11) != 0) {
    return -1;
  }
  if (http_decode_name(req->path + 11, route_name, sizeof(route_name)) != 0) {
    return http_send_json_error(conn, 400, "Bad Request", "invalid file name");
  }
  if (strcmp(req->method, "GET") == 0) {
    return http_send_file(conn, ser_opt, route_name);
  }
  if (strcmp(req->method, "PUT") == 0) {
    return http_handle_file_put(conn, ser_opt, req, route_name);
  }
  if (strcmp(req->method, "DELETE") == 0) {
    return http_handle_file_delete(conn, ser_opt, route_name);
  }
  return http_send_json_error(conn, 405, "Method Not Allowed", "method not allowed");
}

int handle_http_connection(socket_t conn, const server_opt_t *ser_opt) {
  char header_block[HF_HTTP_HEADER_MAX];
  http_request_t req = {0};
  const webui_asset_t *asset = NULL;
  int read_res = http_read_header_block(conn, header_block, sizeof(header_block));
  int parse_res = 0;
  int route_res = 0;

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

  route_res = http_dispatch_exact_route(conn, ser_opt, &req);
  if (route_res != -1) {
    return route_res;
  }

  route_res = http_dispatch_file_route(conn, ser_opt, &req);
  if (route_res != -1) {
    return route_res;
  }

  return http_send_json_error(conn, 404, "Not Found", "route not found");
}
