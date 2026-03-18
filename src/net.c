#include "net.h"
#include <errno.h>
#include <stdio.h>

#ifndef _WIN32
  #include <unistd.h>
#endif


int net_init() {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }
#endif
  return 0;
}

void net_cleanup() {
#ifdef _WIN32
  WSACleanup();
#endif
}

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


void encode_u64_be(uint64_t v, uint8_t out[8]) {
  out[0] = (uint8_t)((v >> 56) & 0xFFu);
  out[1] = (uint8_t)((v >> 48) & 0xFFu);
  out[2] = (uint8_t)((v >> 40) & 0xFFu);
  out[3] = (uint8_t)((v >> 32) & 0xFFu);
  out[4] = (uint8_t)((v >> 24) & 0xFFu);
  out[5] = (uint8_t)((v >> 16) & 0xFFu);
  out[6] = (uint8_t)((v >> 8) & 0xFFu);
  out[7] = (uint8_t)(v & 0xFFu);
}

uint64_t decode_u64_be(const uint8_t in[8]) {
  return ((uint64_t)in[0] << 56) |
         ((uint64_t)in[1] << 48) |
         ((uint64_t)in[2] << 40) |
         ((uint64_t)in[3] << 32) |
         ((uint64_t)in[4] << 24) |
         ((uint64_t)in[5] << 16) |
         ((uint64_t)in[6] << 8) |
         (uint64_t)in[7];
}

void encode_u32_be(uint32_t v, uint8_t out[4]) {
  out[0] = (uint8_t)((v >> 24) & 0xFFu);
  out[1] = (uint8_t)((v >> 16) & 0xFFu);
  out[2] = (uint8_t)((v >> 8) & 0xFFu);
  out[3] = (uint8_t)(v & 0xFFu);
}

uint32_t decode_u32_be(const uint8_t in[4]) {
  return ((uint32_t)in[0] << 24) |
         ((uint32_t)in[1] << 16) |
         ((uint32_t)in[2] << 8) |
         (uint32_t)in[3];
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

int socket_close(socket_t s) {
#ifdef _WIN32
  return closesocket(s);
#else
  return close(s);
#endif
}
