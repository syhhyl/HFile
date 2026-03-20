#include "http.h"

#include "fs.h"
#include "helper.h"
#include "protocol.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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

static const char *http_index_html =
  "<!doctype html>\n"
  "<html lang=\"en\">\n"
  "<head>\n"
  "  <meta charset=\"utf-8\">\n"
  "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
  "  <title>HFile</title>\n"
  "  <link rel=\"stylesheet\" href=\"/styles.css\">\n"
  "</head>\n"
  "<body>\n"
  "  <main class=\"shell\">\n"
  "    <section class=\"hero\">\n"
  "      <h1>HFile</h1>\n"
  "      <p class=\"lede\">If you need to move bytes, you'll like HFile. ❤️</p>\n"
  "    </section>\n"
  "    <section class=\"card stack upload-card\">\n"
  "      <h2>Upload</h2>\n"
  "      <p id=\"upload-status\" class=\"status\"></p>\n"
  "      <div class=\"upload-actions\">\n"
  "        <label for=\"file-input\" class=\"button choose-btn\">Choose File</label>\n"
  "        <button id=\"upload-btn\" type=\"button\" class=\"send-btn\">Send File</button>\n"
  "      </div>\n"
  "      <input id=\"file-input\" type=\"file\" class=\"sr-only\">\n"
  "    </section>\n"
  "    <section class=\"card stack\">\n"
  "      <h2>Message</h2>\n"
  "      <textarea id=\"message-input\" rows=\"4\" maxlength=\"262144\" placeholder=\"Leave a short note\"></textarea>\n"
  "      <button id=\"message-btn\" type=\"button\">Post Message</button>\n"
  "      <p id=\"message-status\" class=\"status\"></p>\n"
  "    </section>\n"
  "    <section class=\"card stack\">\n"
  "      <div class=\"section-head\">\n"
  "        <h2>Files</h2>\n"
  "        <button id=\"refresh-files\" type=\"button\" class=\"quiet\">Refresh</button>\n"
  "      </div>\n"
  "      <div id=\"files\" class=\"list empty\">No files yet.</div>\n"
  "    </section>\n"
  "  </main>\n"
  "  <script src=\"/app.js\"></script>\n"
  "</body>\n"
  "</html>\n";

static const char *http_styles_css =
  ":root {\n"
  "  --bg: #f6efe2;\n"
  "  --bg-alt: #efe3cf;\n"
  "  --ink: #1c1712;\n"
  "  --muted: #6d6156;\n"
  "  --panel: rgba(255, 251, 245, 0.88);\n"
  "  --line: rgba(47, 35, 23, 0.12);\n"
  "  --accent: #bf5a2a;\n"
  "  --accent-2: #1d7c69;\n"
  "  --shadow: 0 16px 40px rgba(43, 31, 20, 0.1);\n"
  "}\n"
  "* { box-sizing: border-box; }\n"
  "body {\n"
  "  margin: 0;\n"
  "  min-height: 100vh;\n"
  "  color: var(--ink);\n"
  "  background: var(--bg);\n"
  "  font-family: \"Iowan Old Style\", \"Palatino Linotype\", \"Book Antiqua\", Georgia, serif;\n"
  "}\n"
  ".shell {\n"
  "  width: min(880px, calc(100vw - 28px));\n"
  "  margin: 0 auto;\n"
  "  padding: 24px 0 40px;\n"
  "}\n"
  ".hero {\n"
  "  padding: 8px 4px 24px;\n"
  "  text-align: center;\n"
  "}\n"
  "h1, h2, p { margin: 0; }\n"
  "h1 {\n"
  "  font-size: clamp(2.6rem, 8vw, 4.8rem);\n"
  "  line-height: 0.96;\n"
  "  letter-spacing: -0.05em;\n"
  "}\n"
  ".lede {\n"
  "  max-width: 36rem;\n"
  "  margin: 12px auto 0;\n"
  "  color: var(--muted);\n"
  "  font-size: 1rem;\n"
  "}\n"
  ".card {\n"
  "  margin-top: 16px;\n"
  "  padding: 18px;\n"
  "  border: 1px solid var(--line);\n"
  "  border-radius: 22px;\n"
  "  background: var(--panel);\n"
  "  box-shadow: var(--shadow);\n"
  "  -webkit-backdrop-filter: blur(12px);\n"
  "  backdrop-filter: blur(12px);\n"
  "}\n"
  ".stack {\n"
  "  display: grid;\n"
  "  gap: 12px;\n"
  "}\n"
  ".upload-card {\n"
  "  min-height: 168px;\n"
  "  align-content: start;\n"
  "}\n"
  ".section-head {\n"
  "  display: flex;\n"
  "  align-items: center;\n"
  "  justify-content: space-between;\n"
  "  gap: 12px;\n"
  "}\n"
  "textarea {\n"
  "  width: 100%;\n"
  "  border: 1px solid var(--line);\n"
  "  border-radius: 16px;\n"
  "  background: rgba(255, 255, 255, 0.6);\n"
  "  padding: 14px;\n"
  "  color: var(--ink);\n"
  "  font: inherit;\n"
  "}\n"
  "textarea { resize: vertical; min-height: 110px; }\n"
  ".button,\n"
  "button {\n"
  "  appearance: none;\n"
  "  border: 0;\n"
  "  border-radius: 999px;\n"
  "  padding: 12px 18px;\n"
  "  display: inline-flex;\n"
  "  align-items: center;\n"
  "  justify-content: center;\n"
  "  color: #fff9f0;\n"
  "  background: linear-gradient(135deg, var(--accent), #d97a35);\n"
  "  font: 700 14px/1 ui-monospace, SFMono-Regular, Menlo, monospace;\n"
  "  letter-spacing: 0.04em;\n"
  "  text-decoration: none;\n"
  "  text-align: center;\n"
  "  cursor: pointer;\n"
  "}\n"
  ".upload-actions {\n"
  "  width: 100%;\n"
  "  display: grid;\n"
  "  grid-template-columns: repeat(2, minmax(0, 1fr));\n"
  "  gap: 12px;\n"
  "  margin-top: auto;\n"
  "}\n"
  ".choose-btn {\n"
  "  color: #17362f;\n"
  "  background: linear-gradient(135deg, #bfe6dc, #8fceb8);\n"
  "}\n"
  ".choose-btn:hover {\n"
  "  background: linear-gradient(135deg, #b1dfd1, #7fc2aa);\n"
  "  box-shadow: 0 10px 22px rgba(29, 124, 105, 0.18);\n"
  "}\n"
  ".send-btn {\n"
  "  background: linear-gradient(135deg, var(--accent), #d97a35);\n"
  "}\n"
  ".send-btn:hover {\n"
  "  background: linear-gradient(135deg, #c96434, #e28a45);\n"
  "  box-shadow: 0 10px 22px rgba(191, 90, 42, 0.22);\n"
  "}\n"
  "button.quiet {\n"
  "  color: #17362f;\n"
  "  background: linear-gradient(135deg, #d9efe7, #b8dccf);\n"
  "  box-shadow: 0 8px 20px rgba(29, 124, 105, 0.16);\n"
  "}\n"
  "button.quiet:hover {\n"
  "  background: linear-gradient(135deg, #cde8de, #a6d1c2);\n"
  "  box-shadow: 0 12px 26px rgba(29, 124, 105, 0.22);\n"
  "}\n"
  ".sr-only {\n"
  "  position: absolute;\n"
  "  width: 1px;\n"
  "  height: 1px;\n"
  "  padding: 0;\n"
  "  margin: -1px;\n"
  "  overflow: hidden;\n"
  "  clip: rect(0, 0, 0, 0);\n"
  "  white-space: nowrap;\n"
  "  border: 0;\n"
  "}\n"
  ".status {\n"
  "  min-height: 3.2em;\n"
  "  color: var(--muted);\n"
  "  font: 500 13px/1.5 ui-monospace, SFMono-Regular, Menlo, monospace;\n"
  "}\n"
  ".list {\n"
  "  display: grid;\n"
  "  gap: 10px;\n"
  "}\n"
  ".list.empty {\n"
  "  color: var(--muted);\n"
  "  font-style: italic;\n"
  "}\n"
  ".row {\n"
  "  display: flex;\n"
  "  align-items: center;\n"
  "  justify-content: space-between;\n"
  "  gap: 12px;\n"
  "  padding: 14px 16px;\n"
  "  border-radius: 18px;\n"
  "  background: rgba(255, 255, 255, 0.6);\n"
  "  border: 1px solid rgba(28, 23, 18, 0.08);\n"
  "}\n"
  ".meta {\n"
  "  display: grid;\n"
  "  gap: 4px;\n"
  "}\n"
  ".meta strong {\n"
  "  word-break: break-word;\n"
  "}\n"
  ".meta span {\n"
  "  color: var(--muted);\n"
  "  font: 500 12px/1.4 ui-monospace, SFMono-Regular, Menlo, monospace;\n"
  "}\n"
  ".row-actions {\n"
  "  display: inline-flex;\n"
  "  align-items: center;\n"
  "  gap: 10px;\n"
  "}\n"
  ".delete,\n"
  ".download {\n"
  "  display: inline-flex;\n"
  "  align-items: center;\n"
  "  justify-content: center;\n"
  "  padding: 10px 16px;\n"
  "  border-radius: 999px;\n"
  "  border: 0;\n"
  "  color: #f4fff9;\n"
  "  text-decoration: none;\n"
  "  font: 700 13px/1 ui-monospace, SFMono-Regular, Menlo, monospace;\n"
  "}\n"
  ".delete {\n"
  "  background: linear-gradient(135deg, #b63838, #dc5b5b);\n"
  "  box-shadow: 0 10px 24px rgba(182, 56, 56, 0.24);\n"
  "}\n"
  ".delete:hover {\n"
  "  background: linear-gradient(135deg, #a92f2f, #d44a4a);\n"
  "  box-shadow: 0 14px 28px rgba(182, 56, 56, 0.3);\n"
  "}\n"
  ".download {\n"
  "  background: #2da44e;\n"
  "  box-shadow: 0 10px 24px rgba(45, 164, 78, 0.24);\n"
  "}\n"
  ".download:hover {\n"
  "  background: #2c974b;\n"
  "  box-shadow: 0 14px 28px rgba(45, 164, 78, 0.3);\n"
  "}\n"
  ".message {\n"
  "  white-space: pre-wrap;\n"
  "  word-break: break-word;\n"
  "}\n"
  "@media (hover: hover) and (pointer: fine) {\n"
  "  .button,\n"
  "  button,\n"
  "  .download {\n"
  "    transition: transform 140ms ease, box-shadow 140ms ease, filter 140ms ease, background 140ms ease;\n"
  "  }\n"
  "  .button:hover,\n"
  "  button:hover,\n"
  "  .download:hover {\n"
  "    transform: translateY(-1px);\n"
  "    filter: brightness(1.03);\n"
  "  }\n"
  "}\n"
  "@media (max-width: 640px) {\n"
  "  .shell { width: min(100vw - 18px, 720px); padding-top: 18px; }\n"
  "  .card {\n"
  "    padding: 16px;\n"
  "    border-radius: 20px;\n"
  "    background: rgba(255, 251, 245, 0.96);\n"
  "    box-shadow: 0 10px 24px rgba(43, 31, 20, 0.08);\n"
  "    -webkit-backdrop-filter: none;\n"
  "    backdrop-filter: none;\n"
  "  }\n"
  "  .upload-actions { width: 100%; }\n"
  "  .row { align-items: flex-start; flex-direction: column; }\n"
  "}\n";

static const char *http_app_js =
  "const filesNode = document.getElementById('files');\n"
  "const uploadStatus = document.getElementById('upload-status');\n"
  "const messageStatus = document.getElementById('message-status');\n"
  "const fileInput = document.getElementById('file-input');\n"
  "const messageInput = document.getElementById('message-input');\n"
  "const fileRows = new Map();\n"
  "\n"
  "function fmtSize(bytes) {\n"
  "  const units = ['B', 'KiB', 'MiB', 'GiB'];\n"
  "  let idx = 0;\n"
  "  let value = Number(bytes);\n"
  "  while (value >= 1024 && idx < units.length - 1) {\n"
  "    value /= 1024;\n"
  "    idx += 1;\n"
  "  }\n"
  "  return `${value.toFixed(idx === 0 ? 0 : 1)} ${units[idx]}`;\n"
  "}\n"
  "\n"
  "function fmtTime(ts) {\n"
  "  const d = new Date(Number(ts) * 1000);\n"
  "  return d.toLocaleString();\n"
  "}\n"
  "\n"
  "function renderFileMeta(file) {\n"
  "  return `${fmtSize(file.size)} · ${fmtTime(file.mtime)}`;\n"
  "}\n"
  "\n"
  "function createFileRow(file) {\n"
  "  const row = document.createElement('div');\n"
  "  row.className = 'row';\n"
  "  row.dataset.fileName = file.name;\n"
  "  row.innerHTML = `\n"
  "    <div class=\"meta\">\n"
  "      <strong></strong>\n"
  "      <span></span>\n"
  "    </div>\n"
  "    <div class=\"row-actions\">\n"
  "      <button class=\"delete\" type=\"button\">Delete</button>\n"
  "      <a class=\"download\" download href=\"/api/files/${encodeURIComponent(file.name)}\">Download</a>\n"
  "    </div>`;\n"
  "  row.querySelector('strong').textContent = file.name;\n"
  "  row.querySelector('span').textContent = renderFileMeta(file);\n"
  "  row.querySelector('.delete').addEventListener('click', async () => {\n"
  "    const res = await fetch(`/api/files/${encodeURIComponent(file.name)}`, {\n"
  "      method: 'DELETE',\n"
  "    });\n"
  "    const payload = await res.json().catch(() => ({}));\n"
  "    if (!res.ok) {\n"
  "      uploadStatus.textContent = payload.error || 'Delete failed.';\n"
  "      return;\n"
  "    }\n"
  "    uploadStatus.textContent = `Deleted ${file.name}.`;\n"
  "    await loadFiles();\n"
  "  });\n"
  "  return row;\n"
  "}\n"
  "\n"
  "function updateFileRow(row, file) {\n"
  "  row.dataset.fileName = file.name;\n"
  "  row.querySelector('strong').textContent = file.name;\n"
  "  row.querySelector('span').textContent = renderFileMeta(file);\n"
  "  const link = row.querySelector('.download');\n"
  "  link.href = `/api/files/${encodeURIComponent(file.name)}`;\n"
  "  link.setAttribute('download', '');\n"
  "}\n"
  "\n"
  "function syncFileList(files) {\n"
  "  if (!files.length) {\n"
  "    fileRows.clear();\n"
  "    filesNode.className = 'list empty';\n"
  "    filesNode.textContent = 'No files yet.';\n"
  "    return;\n"
  "  }\n"
  "\n"
  "  const nextNames = new Set(files.map((file) => file.name));\n"
  "  for (const [name, row] of fileRows.entries()) {\n"
  "    if (!nextNames.has(name)) {\n"
  "      row.remove();\n"
  "      fileRows.delete(name);\n"
  "    }\n"
  "  }\n"
  "\n"
  "  filesNode.className = 'list';\n"
  "  if (filesNode.textContent === 'No files yet.') {\n"
  "    filesNode.textContent = '';\n"
  "  }\n"
  "\n"
  "  files.forEach((file) => {\n"
  "    let row = fileRows.get(file.name);\n"
  "    if (!row) {\n"
  "      row = createFileRow(file);\n"
  "      fileRows.set(file.name, row);\n"
  "    } else {\n"
  "      updateFileRow(row, file);\n"
  "    }\n"
  "    filesNode.appendChild(row);\n"
  "  });\n"
  "}\n"
  "\n"
  "async function loadFiles() {\n"
  "  const res = await fetch('/api/files', { cache: 'no-store' });\n"
  "  if (!res.ok) throw new Error('failed to load files');\n"
  "  const files = await res.json();\n"
  "  syncFileList(files);\n"
  "}\n"
  "\n"
  "async function uploadFile() {\n"
  "  const file = fileInput.files && fileInput.files[0];\n"
  "  if (!file) {\n"
  "    uploadStatus.textContent = 'Pick a file first.';\n"
  "    return;\n"
  "  }\n"
  "  uploadStatus.textContent = `Uploading ${file.name}...`;\n"
  "  const res = await fetch(`/api/files/${encodeURIComponent(file.name)}`, {\n"
  "    method: 'PUT',\n"
  "    headers: { 'Content-Type': 'application/octet-stream' },\n"
  "    body: file,\n"
  "  });\n"
  "  const payload = await res.json().catch(() => ({}));\n"
  "  if (!res.ok) {\n"
  "    uploadStatus.textContent = payload.error || 'Upload failed.';\n"
  "    return;\n"
  "  }\n"
  "  uploadStatus.textContent = `Saved ${file.name}.`;\n"
  "  fileInput.value = '';\n"
  "  await loadFiles();\n"
  "}\n"
  "\n"
  "async function postMessage() {\n"
  "  const message = messageInput.value;\n"
  "  messageStatus.textContent = 'Posting message...';\n"
  "  const res = await fetch('/api/messages', {\n"
  "    method: 'POST',\n"
  "    headers: { 'Content-Type': 'application/json' },\n"
  "    body: JSON.stringify({ message }),\n"
  "  });\n"
  "  const payload = await res.json().catch(() => ({}));\n"
  "  if (!res.ok) {\n"
  "    messageStatus.textContent = payload.error || 'Message failed.';\n"
  "    return;\n"
  "  }\n"
  "  messageStatus.textContent = 'Message saved.';\n"
  "  messageInput.value = '';\n"
  "}\n"
  "\n"
  "fileInput.addEventListener('change', () => {\n"
  "  const file = fileInput.files && fileInput.files[0];\n"
  "  uploadStatus.textContent = file ? `Ready: ${file.name}` : '';\n"
  "});\n"
  "document.getElementById('upload-btn').addEventListener('click', () => {\n"
  "  uploadFile().catch((err) => { uploadStatus.textContent = err.message; });\n"
  "});\n"
  "document.getElementById('message-btn').addEventListener('click', () => {\n"
  "  postMessage().catch((err) => { messageStatus.textContent = err.message; });\n"
  "});\n"
  "document.getElementById('refresh-files').addEventListener('click', () => {\n"
  "  loadFiles().catch((err) => { uploadStatus.textContent = err.message; });\n"
  "});\n"
  "loadFiles().catch((err) => { uploadStatus.textContent = err.message; });\n";

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

static int http_save_file_body(socket_t conn, const server_opt_t *ser_opt,
                               const char *file_name, uint64_t content_length) {
  char full_path[4096];
  char tmp_path[4096];
  char buf[8192];
  int out = -1;
  int exit_code = 1;
  uint64_t remaining = content_length;

  if (fs_join_path(full_path, sizeof(full_path), ser_opt->path, file_name) != 0) {
    return 1;
  }

#ifdef _WIN32
  int pid = _getpid();
#else
  int pid = (int)getpid();
#endif

  for (int attempt = 0; attempt < 16; attempt++) {
    if (fs_make_temp_path(tmp_path, sizeof(tmp_path), full_path, pid, attempt) != 0) {
      return 1;
    }
    out = fs_open_temp_file(tmp_path);
    if (out != -1) {
      break;
    }
    if (errno != EEXIST) {
      perror("open(http_temp)");
      return 1;
    }
  }

  if (out == -1) {
    fprintf(stderr, "failed to create temporary http file\n");
    return 1;
  }

  while (remaining > 0) {
    size_t want = sizeof(buf);
    if ((uint64_t)want > remaining) {
      want = (size_t)remaining;
    }

    ssize_t n = recv(conn, buf, want, 0);
    if (n < 0) {
      sock_perror("recv(http_body)");
      goto CLEANUP;
    }
    if (n == 0) {
      fprintf(stderr, "http upload ended early\n");
      goto CLEANUP;
    }

    if (fs_write_all(out, buf, (size_t)n) != n) {
      perror("write(http_body)");
      goto CLEANUP;
    }
    remaining -= (uint64_t)n;
  }

  if (fs_close(out) != 0) {
    perror("close(http_temp)");
    out = -1;
    goto CLEANUP;
  }
  out = -1;

  if (fs_finalize_temp_file(tmp_path, full_path, NULL) != 0) {
    perror("rename(http_temp)");
    goto CLEANUP;
  }

  exit_code = 0;

CLEANUP:
  if (out != -1) fs_close(out);
  if (exit_code != 0) {
    fs_remove_quiet(tmp_path);
  }
  return exit_code;
}

static int http_send_file(socket_t conn, const server_opt_t *ser_opt,
                          const char *file_name) {
  char path[4096];
  char header[1024];
  char buf[8192];
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

  for (;;) {
    ssize_t nr = fs_read(fd, buf, sizeof(buf));
    if (nr < 0) {
      perror("read(http_download)");
      goto CLEANUP;
    }
    if (nr == 0) {
      break;
    }
    if (send_all(conn, buf, (size_t)nr) != nr) {
      goto CLEANUP;
    }
  }

  exit_code = 0;

CLEANUP:
  if (fd != -1) fs_close(fd);
  return exit_code;
}

static int http_handle_index(socket_t conn) {
  return http_send_response(conn, 200, "OK", "text/html; charset=utf-8",
                            http_index_html, strlen(http_index_html), NULL);
}

static int http_handle_app_js(socket_t conn) {
  return http_send_response(conn, 200, "OK",
                            "application/javascript; charset=utf-8",
                            http_app_js, strlen(http_app_js), NULL);
}

static int http_handle_styles(socket_t conn) {
  return http_send_response(conn, 200, "OK", "text/css; charset=utf-8",
                            http_styles_css, strlen(http_styles_css), NULL);
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

  body = (char *)malloc((size_t)req->content_length + 1u);
  if (body == NULL) {
    return http_send_json_error(conn, 500, "Internal Server Error", "allocation failed");
  }

  size_t total = 0;
  while (total < (size_t)req->content_length) {
    ssize_t n = recv(conn, body + total, (size_t)req->content_length - total, 0);
    if (n < 0) {
      sock_perror("recv(http_message)");
      goto CLEANUP;
    }
    if (n == 0) {
      goto CLEANUP;
    }
    total += (size_t)n;
  }
  body[req->content_length] = '\0';

  if (http_parse_message_json(body, (size_t)req->content_length, &message) != 0) {
    (void)http_send_json_error(conn, 400, "Bad Request", "invalid message payload");
    goto CLEANUP;
  }
  (void)ser_opt;
  printf("msg: %s\n", message);
  fflush(stdout);

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

static int http_handle_file_put(socket_t conn, const server_opt_t *ser_opt,
                                const http_request_t *req, const char *file_name) {
  http_buf_t response = {0};
  char path[4096];
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

  if (http_save_file_body(conn, ser_opt, file_name, req->content_length) != 0) {
    return http_send_json_error(conn, 500, "Internal Server Error", "failed to save file");
  }

  if (fs_join_path(path, sizeof(path), ser_opt->path, file_name) != 0 ||
      http_get_file_info(path, &size, &mtime) != 0) {
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

static int http_handle_connection(socket_t conn, const server_opt_t *ser_opt) {
  char header_block[HF_HTTP_HEADER_MAX];
  char route_name[HF_PROTOCOL_MAX_FILE_NAME_LEN + 1u];
  http_request_t req = {0};
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

  if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/") == 0) {
    return http_handle_index(conn);
  }
  if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/app.js") == 0) {
    return http_handle_app_js(conn);
  }
  if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/styles.css") == 0) {
    return http_handle_styles(conn);
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

int http_server(socket_t listener, const server_opt_t *ser_opt) {
  int exit_code = 0;

  for (;;) {
#ifdef _WIN32
    SOCKET conn = accept(listener, NULL, NULL);
    if (conn == INVALID_SOCKET) {
      if (WSAGetLastError() == WSAEINTR) continue;
#else
    int conn = accept(listener, NULL, NULL);
    if (conn < 0) {
      if (errno == EINTR) continue;
#endif
      sock_perror("accept(http)");
      exit_code = 1;
      continue;
    }

    (void)http_handle_connection(conn, ser_opt);
    socket_close(conn);
  }

  return exit_code;
}
