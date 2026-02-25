#include "net.h"


ssize_t send_all(
#ifdef _WIN32
  SOCKET sock,
#else
  int sock,
#endif
  const void *data, size_t len) {
  size_t total = 0;
  const char *p = data;

#ifndef _WIN32
  int flags = 0;
  #ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
  #endif
#endif

  while (total < len) {
#ifdef _WIN32
    int n = send(sock, p+total, (int)(len-total), 0);
    if (n == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAEINTR) continue;
      return -1;
    }
#else
    ssize_t n = send(sock, p+total, len-total, flags);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
#endif
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  
  return (ssize_t)total;
}

ssize_t recv_all(
#ifdef _WIN32
  SOCKET sock,
#else
  int sock,
#endif
  void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  size_t total = 0;

  while (total < len) {
#ifdef _WIN32
    int n = recv(sock, (char *)p + total, (int)(len - total), 0);
    if (n == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAEINTR) continue;
      return -1;
    }
#else
    ssize_t n = recv(sock, p + total, len - total, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
#endif
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  return (ssize_t)total;
}

void sock_perror(const char *msg) {
#ifdef _WIN32
  int err = WSAGetLastError();
  char *sys = NULL;
  DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD n = FormatMessageA(flags, NULL, (DWORD)err,
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           (LPSTR)&sys, 0, NULL);
  if (n != 0 && sys != NULL) {
    while (n > 0 && (sys[n - 1] == '\r' || sys[n - 1] == '\n')) {
      sys[n - 1] = '\0';
      n--;
    }
    fprintf(stderr, "%s: %s (WSA=%d)\n", msg, sys, err);
    LocalFree(sys);
  } else {
    fprintf(stderr, "%s: WSA error %d\n", msg, err);
  }
#else
  perror(msg);
#endif
}