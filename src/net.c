#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

bool is_socket_invalid(socket_t sock) {
  return sock < 0;
}

ssize_t send_all(socket_t sock, const void *data, size_t len) {
  const char *p = data;
  size_t remaining = len;

  int flags = 0;
  #ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
  #endif

  while (remaining > 0) {
    ssize_t n = send(sock, p, remaining, flags);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) break;
    p += n;
    remaining -= (size_t)n;
  }
  return (ssize_t)(len - remaining);
}

ssize_t recv_all(socket_t sock, void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  size_t remaining = len;

  while (remaining > 0) {
    ssize_t n = recv(sock, p, remaining, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) break;
    p += n;
    remaining -= (size_t)n;
  }
  return (ssize_t)(len - remaining);
}

int socket_close(socket_t s) {
  if (s < 0) return 0;
  return close(s);
}
