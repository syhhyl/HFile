#define _GNU_SOURCE

#include "net.h"
#include "node.h"
#include "protocol.h"
#include "discovery.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>

#if defined(__linux__)
  #include <sys/sendfile.h>
#elif defined(__APPLE__)
  #include <sys/socket.h>
  #include <sys/uio.h>
#endif

#define CHUNK (1024 * 1024)

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

static int reply(socket_t sock, const res_frame_t *f) {
  if (is_socket_invalid(sock) || !f) return 1;
  if (f->phase != PROTO_PHASE_READY && f->phase != PROTO_PHASE_FINAL) return 1;
  if (f->status != PROTO_STATUS_OK &&
      f->status != PROTO_STATUS_REJECTED &&
      f->status != PROTO_STATUS_FAILED) return 1;
  if (f->error_code > PROTOCOL_ERR_EOF) return 1;
  if ((f->status == PROTO_STATUS_OK) != (f->error_code == PROTOCOL_OK)) return 1;

  uint8_t buf[4];
  buf[0] = f->phase;
  buf[1] = f->status;
  uint16_t ec = htons(f->error_code);
  memcpy(buf + 2, &ec, 2);
  return send_all(sock, buf, 4) == 4 ? 0 : 1;
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

/* zero-copy recv */

static int recv_body(socket_t sock, int fd, uint64_t size) {
  if (size == 0) return 0;
  if (is_socket_invalid(sock) || fd < 0) return 1;

#if defined(__linux__)
  int pfd[2] = {-1, -1};
  uint64_t remain = size;
  int ok = 0;

  if (pipe(pfd)) return 1;

  while (remain) {
    size_t n = (uint64_t)CHUNK < remain ? CHUNK : (size_t)remain;
    ssize_t r = splice(sock, NULL, pfd[1], NULL, n, SPLICE_F_MOVE | SPLICE_F_MORE);
    if (r < 0) { if (errno == EINTR) continue; goto done; }
    if (r == 0) goto done;

    ssize_t pleft = r;
    while (pleft > 0) {
      ssize_t w = splice(pfd[0], NULL, fd, NULL, (size_t)pleft, SPLICE_F_MOVE | SPLICE_F_MORE);
      if (w < 0) { if (errno == EINTR) continue; goto done; }
      if (w == 0) goto done;
      pleft -= w;
      remain -= (uint64_t)w;
    }
  }
  ok = 1;

done:
  if (pfd[0] >= 0) close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
  return ok ? 0 : 1;

#else
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
#endif
}

/* node_recv */

int node_recv(const char *dir, uint16_t port) {
  int opt = 1;
  struct sockaddr_in addr = {0};
  socket_t tcp, disc = -1;

  if (!dir || !*dir) return 1;

  tcp = socket(AF_INET, SOCK_STREAM, 0);
  setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(tcp, (struct sockaddr *)&addr, sizeof(addr));
  listen(tcp, 1);

  int disc_ok = (port < 65535u && discovery_open(&disc, (uint16_t)(port + 1u)) == 0);

  fprintf(stdout, "HFile node ready\n  Receive Dir  %s\n  Port  %u\n  PID  %ld\n",
          dir, (unsigned)port, (long)getpid());
  if (disc_ok) fprintf(stdout, "  Discovery on %u\n", (unsigned)(port + 1));
  fflush(stdout);

  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds); FD_SET(tcp, &rfds);
    int nfds = (int)tcp + 1;
    if (disc_ok) { FD_SET(disc, &rfds); if (disc > nfds - 1) nfds = (int)disc + 1; }

    if (select(nfds, &rfds, NULL, NULL, NULL) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (disc_ok && FD_ISSET(disc, &rfds))
      discovery_handle_query(disc, port);
    if (!FD_ISSET(tcp, &rfds)) continue;

    socket_t conn = accept(tcp, NULL, NULL);
    if (conn < 0) continue;

    /* recv one file */
    uint8_t hdr[13];
    char *name = NULL;
    uint64_t fsize, hdr_payload;
    char path[4096], tmp[4096];
    int fd = -1, ok = 0;

    if (recv_all(conn, hdr, 13) != 13) goto shut;

    if (((uint16_t)hdr[0] << 8 | hdr[1]) != HF_PROTOCOL_MAGIC) goto shut;
    if (hdr[2] != HF_PROTOCOL_VERSION) goto shut;
    if (hdr[3] != HF_MSG_TYPE_SEND_FILE) goto shut;
    if (hdr[4] != HF_MSG_FLAG_NONE) goto shut;
    hdr_payload = be64_read(hdr + 5);

    if (hdr_payload < (uint64_t)(2 + 1 + 8)) {
      reply(conn, &(res_frame_t){PROTO_PHASE_READY, PROTO_STATUS_REJECTED, 5});
      goto shut;
    }

    /* read prefix: name_len(2B) + name + content_size(8B) */
    {
      uint16_t nbe;
      if (recv_all(conn, &nbe, 2) != 2) goto shut;
      uint16_t nlen = ntohs(nbe);
      if (nlen == 0 || nlen > 255) goto shut;
      name = malloc((size_t)nlen + 1);
      if (!name) goto shut;
      if (recv_all(conn, name, nlen) != (ssize_t)nlen) { free(name); name = NULL; goto shut; }
      name[nlen] = '\0';

      uint8_t s8[8];
      if (recv_all(conn, s8, 8) != 8) { free(name); name = NULL; goto shut; }
      fsize = be64_read(s8);

      uint64_t prefix = 2 + nlen + 8;
      if (hdr_payload != prefix + fsize) {
        reply(conn, &(res_frame_t){PROTO_PHASE_READY, PROTO_STATUS_REJECTED, 8});
        goto free_name;
      }
    }

    if (!ok_name(name)) {
      reply(conn, &(res_frame_t){PROTO_PHASE_READY, PROTO_STATUS_REJECTED, 7});
      goto free_name;
    }

    if (reply(conn, &(res_frame_t){PROTO_PHASE_READY, PROTO_STATUS_OK, 0}))
      goto free_name;

    if (join_path(path, sizeof(path), dir, name)) goto fail;

    for (int i = 0; i < 3 && fd < 0; i++) {
      tmp_path(tmp, sizeof(tmp), path, (int)getpid(), i);
      fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC | O_EXCL, 0644);
    }
    if (fd < 0) goto fail;

    if (recv_body(conn, fd, fsize)) {
      close(fd); remove(tmp); goto fail;
    }
    close(fd); fd = -1;
    if (rename(tmp, path)) { remove(tmp); goto fail; }

    ok = 1;

  fail:
    reply(conn, &(res_frame_t){PROTO_PHASE_FINAL,
      ok ? PROTO_STATUS_OK : PROTO_STATUS_FAILED, ok ? 0 : 9});

  free_name:
    free(name);
    if (ok) fprintf(stdout, "received  %s  %llu bytes\n", name, (unsigned long long)fsize);
  shut:
    socket_close(conn);
  }

  socket_close(tcp);
  if (disc_ok) discovery_close(disc);
  return 0;
}

/* node_send */

int node_send(const char *path, const char *ip, uint16_t port) {
  socket_t sock = -1;
  int src = -1, ret = 1;
  const char *name;

  {
    const char *slash = strrchr(path, '/');
    name = slash ? slash + 1 : path;
  }

  size_t nlen = strlen(name);
  if (nlen == 0 || nlen > 255) return 1;

  src = open(path, O_RDONLY, 0);
  if (src < 0) return 1;

  struct stat st;
  fstat(src, &st);
  if (!S_ISREG(st.st_mode) || st.st_size < 0) goto exit;
  uint64_t fsize = (uint64_t)st.st_size;

  const char *peer = ip;
  uint16_t pport = port;
  if (!peer) {
    char found[64];
    if (discovery_find_node((uint16_t)(port + 1), found, sizeof(found), &pport)) goto exit;
    peer = found;
  }

  sock = socket(AF_INET, SOCK_STREAM, 0);
  { struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(pport);
    inet_pton(AF_INET, peer, &addr.sin_addr);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto exit; }

  /* encode preamble: header(13B) + prefix(name_len+name+size) */
  {
    size_t prefix = 2 + nlen + 8;
    uint8_t buf[13 + 2 + 255 + 8], *p = buf;

    uint16_t mg = htons(HF_PROTOCOL_MAGIC);
    memcpy(p, &mg, 2); p += 2;
    *p++ = HF_PROTOCOL_VERSION;
    *p++ = HF_MSG_TYPE_SEND_FILE;
    *p++ = HF_MSG_FLAG_NONE;
    be64_write(p, (uint64_t)prefix + fsize); p += 8;

    uint16_t nbe = htons((uint16_t)nlen);
    memcpy(p, &nbe, 2); p += 2;
    memcpy(p, name, nlen); p += nlen;
    be64_write(p, fsize);

    if (send_all(sock, buf, 13 + prefix) < (ssize_t)(13 + prefix)) goto exit;
  }

  /* wait READY */
  {
    uint8_t res[4];
    if (recv_all(sock, res, 4) != 4) goto exit;
    if (res[0] != PROTO_PHASE_READY || res[1] != PROTO_STATUS_OK) goto exit;
  }

  if (send_body(sock, src, fsize)) goto exit;

  /* wait FINAL */
  {
    uint8_t res[4];
    if (recv_all(sock, res, 4) != 4) goto exit;
    if (res[0] != PROTO_PHASE_FINAL || res[1] != PROTO_STATUS_OK) goto exit;
  }

  ret = 0;
exit:
  if (src >= 0) close(src);
  socket_close(sock);
  return ret;
}
