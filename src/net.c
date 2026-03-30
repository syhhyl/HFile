#include "net.h"
#include "fs.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
  #include <unistd.h>
  #if defined(__linux__)
    #include <sys/sendfile.h>
  #elif defined(__APPLE__)
    #include <sys/socket.h>
    #include <sys/uio.h>
  #endif
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

void socket_init(socket_t *s) {
#ifdef _WIN32
  *s = INVALID_SOCKET;
#else
  *s = -1;
#endif
}

int socket_close(socket_t s) {
#ifdef _WIN32
  if (s == INVALID_SOCKET) return 0;
  return closesocket(s);
#else
  if (s < 0) return 0;
  return close(s);
#endif
}

int net_wait_readable(socket_t sock, uint32_t timeout_ms, int *ready_out) {
  if (ready_out == NULL) {
    return 1;
  }

  *ready_out = 0;

#ifdef _WIN32
  if (sock == INVALID_SOCKET) {
    return 1;
  }
#else
  if (sock < 0) {
    return 1;
  }
#endif

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(sock, &readfds);

  struct timeval tv;
  tv.tv_sec = (long)(timeout_ms / 1000u);
  tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

#ifdef _WIN32
  int rc = select(0, &readfds, NULL, NULL, &tv);
  if (rc == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err == WSAEINTR) {
      return 0;
    }
    return 1;
  }
#else
  int rc = select(sock + 1, &readfds, NULL, NULL, &tv);
  if (rc < 0) {
    if (errno == EINTR) {
      return 0;
    }
    return 1;
  }
#endif

  if (rc == 0) {
    return 0;
  }

  *ready_out = FD_ISSET(sock, &readfds) ? 1 : 0;
  return 0;
}

int net_primary_ipv4(char *out, size_t out_cap) {
  struct sockaddr_in remote = {0};
  struct sockaddr_in local = {0};
  socket_t sock;
  socklen_t local_len = (socklen_t)sizeof(local);

  if (out == NULL || out_cap == 0) {
    return 1;
  }

  socket_init(&sock);
  sock = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
  if (sock == INVALID_SOCKET) {
#else
  if (sock < 0) {
#endif
    return 1;
  }

  remote.sin_family = AF_INET;
  remote.sin_port = htons(53);
  if (inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr) != 1) {
    socket_close(sock);
    return 1;
  }

  (void)connect(sock, (struct sockaddr *)&remote, sizeof(remote));
  if (getsockname(sock, (struct sockaddr *)&local, &local_len) != 0) {
    socket_close(sock);
    return 1;
  }

  if (inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)out_cap) == NULL) {
    socket_close(sock);
    return 1;
  }

  socket_close(sock);
  if (strcmp(out, "0.0.0.0") == 0 || strcmp(out, "127.0.0.1") == 0) {
    return 1;
  }
  return 0;
}

net_send_file_result_t net_send_file_all(socket_t sock,
                                         int in_fd,
                                         uint64_t content_size) {
  if (content_size == 0) {
    return NET_SEND_FILE_OK;
  }

#ifdef _WIN32
  (void)sock;
  (void)in_fd;
  (void)content_size;
  return NET_SEND_FILE_UNSUPPORTED;
#else
  if (sock < 0 || in_fd < 0) {
    return NET_SEND_FILE_INVALID_ARGUMENT;
  }

  uint64_t remaining = content_size;

  #if defined(__linux__)
    off_t offset = 0;
    while (remaining > 0) {
      size_t want = (remaining > (uint64_t)SIZE_MAX)
                      ? (size_t)SIZE_MAX
                      : (size_t)remaining;
      ssize_t n = sendfile(sock, in_fd, &offset, want);
      if (n < 0) {
        if ((errno == EINVAL || errno == ENOSYS || errno == ENOTSUP) &&
            offset == 0) {
          return NET_SEND_FILE_UNSUPPORTED;
        }
        if (errno == EINTR) continue;
        return NET_SEND_FILE_IO;
      }
      if (n == 0) {
        return NET_SEND_FILE_SOURCE_CHANGED;
      }
      remaining -= (uint64_t)n;
    }
    return NET_SEND_FILE_OK;
  #elif defined(__APPLE__)
    off_t offset = 0;
    while (remaining > 0) {
      off_t want = (remaining > (uint64_t)INT64_MAX)
                     ? (off_t)INT64_MAX
                     : (off_t)remaining;
      off_t sent = want;
      int rc = sendfile(in_fd, sock, offset, &sent, NULL, 0);
      if (rc == 0) {
        if (sent <= 0) {
          return NET_SEND_FILE_SOURCE_CHANGED;
        }
        remaining -= (uint64_t)sent;
        offset += sent;
        continue;
      }

      if (sent > 0) {
        remaining -= (uint64_t)sent;
        offset += sent;
      }

      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      if ((errno == EINVAL || errno == ENOTSUP || errno == ENOSYS) &&
          offset == 0 && sent == 0) {
        return NET_SEND_FILE_UNSUPPORTED;
      }
      if (errno == EPIPE) {
        return NET_SEND_FILE_IO;
      }
      return NET_SEND_FILE_IO;
    }
    return NET_SEND_FILE_OK;
  #else
    (void)sock;
    (void)in_fd;
    (void)content_size;
    return NET_SEND_FILE_UNSUPPORTED;
  #endif
#endif
}

static net_send_file_result_t net_send_file_buffered(socket_t sock,
                                                     int in_fd,
                                                     uint64_t content_size) {
  int exit_code = NET_SEND_FILE_OK;
  char *buf = NULL;
  uint64_t remaining = content_size;

  if (sock < 0 || in_fd < 0) {
    return NET_SEND_FILE_INVALID_ARGUMENT;
  }

  if (content_size == 0) {
    return NET_SEND_FILE_OK;
  }

  buf = (char *)malloc(CHUNK_SIZE);
  if (buf == NULL) {
    return NET_SEND_FILE_IO;
  }

  while (remaining > 0) {
    size_t want = CHUNK_SIZE;
    if ((uint64_t)want > remaining) {
      want = (size_t)remaining;
    }

    ssize_t nr = fs_read(in_fd, buf, want);
    if (nr < 0) {
      exit_code = NET_SEND_FILE_IO;
      goto CLEANUP;
    }
    if (nr == 0) {
      exit_code = NET_SEND_FILE_SOURCE_CHANGED;
      goto CLEANUP;
    }
    if (send_all(sock, buf, (size_t)nr) != nr) {
      exit_code = NET_SEND_FILE_IO;
      goto CLEANUP;
    }

    remaining -= (uint64_t)nr;
  }

CLEANUP:
  free(buf);
  return (net_send_file_result_t)exit_code;
}

net_send_file_result_t net_send_file_best_effort(socket_t sock,
                                                 int in_fd,
                                                 uint64_t content_size) {
  net_send_file_result_t res = net_send_file_all(sock, in_fd, content_size);
  if (res != NET_SEND_FILE_UNSUPPORTED) {
    return res;
  }

  return net_send_file_buffered(sock, in_fd, content_size);
}
