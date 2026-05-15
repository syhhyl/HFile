#ifdef __linux__
  #ifndef _GNU_SOURCE
    #define _GNU_SOURCE
  #endif
#endif

#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

bool is_socket_invalid(socket_t sock) {
  return sock < 0;
}

ssize_t send_all(socket_t sock, const void *data, size_t len) {
  size_t total = 0;
  const char *p = data;

  int flags = 0;
  #ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
  #endif

  while (total < len) {
    ssize_t n = send(sock, p + total, len - total, flags);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  return (ssize_t)total;
}

ssize_t recv_all(socket_t sock, void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  size_t total = 0;

  while (total < len) {
    ssize_t n = recv(sock, p + total, len - total, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  return (ssize_t)total;
}

int socket_close(socket_t s) {
  if (s < 0) return 0;
  return close(s);
}
