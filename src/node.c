#include "net.h"
#include "node.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(__linux__)
  #include <sys/sendfile.h>
#elif defined(__APPLE__)
  #include <sys/socket.h>
  #include <sys/uio.h>
#endif

#define CHUNK (1024 * 1024)
#define NAME_BYTES 256
#define PREAMBLE_BYTES (13 + 2 + NAME_BYTES + 8)

/* helpers */

static uint64_t be64_read(const uint8_t *p) {
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
         ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
         ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
         ((uint64_t)p[6] << 8)  |  (uint64_t)p[7];
}

static void be64_write(uint8_t *p, uint64_t v) {
  p[0] = (uint8_t)(v >> 56); p[1] = (uint8_t)(v >> 48);
  p[2] = (uint8_t)(v >> 40); p[3] = (uint8_t)(v >> 32);
  p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
  p[6] = (uint8_t)(v >> 8);  p[7] = (uint8_t)v;
}

static int ok_name(const char *name) {
  if (!name || !*name) return 0;
  if (strchr(name, '/') || strchr(name, '\\')) return 0;
  if (strstr(name, "..")) return 0;
  return 1;
}

static int join_path(char *out, size_t cap, const char *dir, const char *name) {
  size_t dlen = strlen(dir);
  return snprintf(out, cap, "%s%s%s", dir, (dlen && dir[dlen-1]=='/') ? "" : "/", name) >= (int)cap;
}

static int tmp_path(char *out, size_t cap, const char *final, int pid, int attempt) {
  return snprintf(out, cap, "%s.tmp.%d.%d", final, pid, attempt) >= (int)cap;
}

/* response frame */

static int reply(socket_t sock, uint8_t phase, uint8_t status) {
  if (is_socket_invalid(sock)) return 1;
  if (phase != PROTO_PHASE_READY && phase != PROTO_PHASE_FINAL) return 1;
  if (status != PROTO_STATUS_OK &&
      status != PROTO_STATUS_REJECTED &&
      status != PROTO_STATUS_FAILED) return 1;

  uint8_t buf[2];
  buf[0] = phase;
  buf[1] = status;
  return send_all(sock, buf, sizeof(buf)) == (ssize_t)sizeof(buf) ? 0 : 1;
}

/* zero-copy send */

static int send_body(socket_t sock, int fd, uint64_t size) {
  if (size == 0) return 0;
  if (is_socket_invalid(sock) || fd < 0) return 1;

#if defined(__linux__)
  off_t offset = 0;
  uint64_t remain = size;
  while (remain) {
    size_t n = (remain > (uint64_t)SIZE_MAX) ? (size_t)SIZE_MAX : (size_t)remain;
    ssize_t r = sendfile(sock, fd, &offset, n);
    if (r < 0) { if (errno == EINTR) continue; return 1; }
    if (r == 0) return 1;
    remain -= (uint64_t)r;
  }
  return 0;

#elif defined(__APPLE__)
  off_t offset = 0;
  uint64_t remain = size;
  while (remain) {
    off_t n = (off_t)(remain > (uint64_t)INT64_MAX ? (uint64_t)INT64_MAX : remain);
    off_t sent = n;
    int rc = sendfile(fd, sock, offset, &sent, NULL, 0);
    if (rc == 0) {
      if (sent <= 0) return 1;
      remain -= (uint64_t)sent; offset += sent;
      continue;
    }
    if (sent > 0) { remain -= (uint64_t)sent; offset += sent; }
    if (errno == EINTR) continue;
    return 1;
  }
  return 0;

#else
  char *buf = malloc(CHUNK);
  if (!buf) return 1;
  uint64_t remain = size;
  int ok = 0;
  while (remain) {
    size_t n = (uint64_t)CHUNK < remain ? CHUNK : (size_t)remain;
    ssize_t r = read(fd, buf, n);
    if (r < 0) { if (errno == EINTR) continue; break; }
    if (r == 0) break;
    if (send_all(sock, buf, (size_t)r) != r) break;
    remain -= (uint64_t)r;
  }
  ok = (remain == 0);
  free(buf);
  return ok ? 0 : 1;
#endif
}

/* buffered recv */

static int recv_body(socket_t sock, int fd, uint64_t size) {
  if (size == 0) return 0;
  if (is_socket_invalid(sock) || fd < 0) return 1;

  char stack[8192], *heap = NULL, *buf = stack;

  if (size > sizeof(stack)) {
    heap = malloc(CHUNK);
    if (!heap) return 1;
    buf = heap;
  }

  uint64_t remain = size;
  int ok = 0;

  while (remain) {
    size_t n = (uint64_t)CHUNK < remain ? CHUNK : (size_t)remain;
    ssize_t r = recv(sock, buf, n, 0);
    if (r < 0) { if (errno == EINTR) continue; break; }
    if (r == 0) break;

    const char *wp = buf;
    size_t wleft = (size_t)r;
    while (wleft) {
      ssize_t w = write(fd, wp, wleft);
      if (w < 0) { if (errno == EINTR) continue; goto done2; }
      wp += w; wleft -= (size_t)w;
    }
    remain -= (uint64_t)r;
  }
  ok = (remain == 0);
done2:
  free(heap);
  return ok ? 0 : 1;
}

/* node_recv */

int node_recv(const char *dir, uint16_t port) {
  int opt = 1;
  struct sockaddr_in addr = {0};
  socket_t tcp;
  struct stat st;

  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    fprintf(stderr, "invalid receive directory\n");
    return 1;
  }

  tcp = socket(AF_INET, SOCK_STREAM, 0);
  if (is_socket_invalid(tcp)) {
    fprintf(stderr, "failed to start receiver\n");
    return 1;
  }
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0 ||
      bind(tcp, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(tcp, 1) != 0) {
    fprintf(stderr, "failed to start receiver\n");
    socket_close(tcp);
    return 1;
  }

  fprintf(stdout, "HFile node ready\n  Receive Dir  %s\n  Port  %u\n  PID  %ld\n",
          dir, (unsigned)port, (long)getpid());
  fflush(stdout);

  for (;;) {
    socket_t conn = accept(tcp, NULL, NULL);
    if (conn < 0) continue;

    /* recv one file */
    uint8_t pre[PREAMBLE_BYTES];
    char name[NAME_BYTES + 1];
    uint64_t fsize, hdr_payload;
    char path[4096], tmp[4096];
    int fd = -1, ok = 0;

    if (recv_all(conn, pre, sizeof(pre)) != (ssize_t)sizeof(pre)) goto shut;

    uint16_t nlen = ((uint16_t)pre[13] << 8) | pre[14];
    hdr_payload = be64_read(pre + 5);
    fsize = be64_read(pre + 13 + 2 + NAME_BYTES);
    if (((uint16_t)pre[0] << 8 | pre[1]) != HF_PROTOCOL_MAGIC ||
        pre[2] != HF_PROTOCOL_VERSION ||
        pre[3] != HF_PROTOCOL_MSG_TYPE_SEND_FILE ||
        pre[4] != HF_PROTOCOL_MSG_FLAG_NONE ||
        nlen == 0 || nlen >= NAME_BYTES ||
        hdr_payload != (uint64_t)(2 + NAME_BYTES + 8) + fsize) {
      goto reject;
    }

    memcpy(name, pre + 15, nlen);
    name[nlen] = '\0';
    if (!ok_name(name)) goto reject;

    if (reply(conn, PROTO_PHASE_READY, PROTO_STATUS_OK))
      goto shut;

    if (join_path(path, sizeof(path), dir, name)) goto final;

    for (int i = 0; i < 3 && fd < 0; i++) {
      tmp_path(tmp, sizeof(tmp), path, (int)getpid(), i);
      fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC | O_EXCL, 0644);
    }
    if (fd < 0) goto final;

    if (recv_body(conn, fd, fsize)) {
      close(fd); remove(tmp); goto final;
    }
    close(fd); fd = -1;
    if (rename(tmp, path)) { remove(tmp); goto final; }

    ok = 1;

  final:
    reply(conn, PROTO_PHASE_FINAL, ok ? PROTO_STATUS_OK : PROTO_STATUS_FAILED);

    if (ok) fprintf(stdout, "received  %s  %llu bytes\n", name, (unsigned long long)fsize);
  shut:
    socket_close(conn);
    continue;

  reject:
    reply(conn, PROTO_PHASE_READY, PROTO_STATUS_REJECTED);
    socket_close(conn);
    continue;
  }

}

/* node_send */

int node_send(const char *path, const char *ip, uint16_t port) {
  socket_t sock = -1;
  int src = -1, ret = 1;
  struct sockaddr_in addr = {0};
  const char *name;
  uint16_t pport = port;

  addr.sin_family = AF_INET;
  addr.sin_port = htons(pport);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    fprintf(stderr, "invalid address\n");
    return 1;
  }

  {
    const char *slash = strrchr(path, '/');
    name = slash ? slash + 1 : path;
  }

  size_t nlen = strlen(name);
  if (nlen == 0 || nlen >= NAME_BYTES) return 1;

  src = open(path, O_RDONLY, 0);
  if (src < 0) {
    fprintf(stderr, "failed to open file: %s\n", path);
    return 1;
  }

  struct stat st;
  if (fstat(src, &st) != 0) goto exit;
  if (!S_ISREG(st.st_mode) || st.st_size < 0) goto exit;
  uint64_t fsize = (uint64_t)st.st_size;

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto exit;

  /* encode preamble: header(13B) + prefix(name_len+name+size) */
  {
    uint8_t buf[PREAMBLE_BYTES] = {0}, *p = buf;

    uint16_t mg = htons(HF_PROTOCOL_MAGIC);
    memcpy(p, &mg, 2); p += 2;
    *p++ = HF_PROTOCOL_VERSION;
    *p++ = HF_PROTOCOL_MSG_TYPE_SEND_FILE;
    *p++ = HF_PROTOCOL_MSG_FLAG_NONE;
    be64_write(p, (uint64_t)(2 + NAME_BYTES + 8) + fsize); p += 8;

    uint16_t nbe = htons((uint16_t)nlen);
    memcpy(p, &nbe, 2); p += 2;
    memcpy(p, name, nlen); p += NAME_BYTES;
    be64_write(p, fsize);

    if (send_all(sock, buf, sizeof(buf)) < (ssize_t)sizeof(buf)) goto exit;
  }

  /* wait READY */
  {
    uint8_t res[2];
    if (recv_all(sock, res, sizeof(res)) != (ssize_t)sizeof(res)) goto exit;
    if (res[0] != PROTO_PHASE_READY || res[1] != PROTO_STATUS_OK) goto exit;
  }

  if (send_body(sock, src, fsize)) goto exit;

  /* wait FINAL */
  {
    uint8_t res[2];
    if (recv_all(sock, res, sizeof(res)) != (ssize_t)sizeof(res)) goto exit;
    if (res[0] != PROTO_PHASE_FINAL || res[1] != PROTO_STATUS_OK) goto exit;
  }

  ret = 0;
exit:
  if (src >= 0) close(src);
  socket_close(sock);
  return ret;
}
