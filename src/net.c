#ifdef __linux__
  #ifndef _GNU_SOURCE
    #define _GNU_SOURCE
  #endif
#endif

#include "net.h"
#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
  #include <sys/sendfile.h>
#elif defined(__APPLE__)
  #include <sys/socket.h>
  #include <sys/uio.h>
#endif

bool is_socket_invalid(socket_t sock) {
  return sock < 0;
}

ssize_t send_all(
  socket_t sock,
  const void *data, size_t len) {
  size_t total = 0;
  const char *p = data;

  int flags = 0;
  #ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
  #endif

  while (total < len) {
    ssize_t n = send(sock, p+total, len-total, flags);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }

  return (ssize_t)total;
}

ssize_t recv_all(
  socket_t sock,
  void *buf, size_t len) {
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

void sock_perror(const char *msg) {
  perror(msg);
}

void socket_init(socket_t *s) {
  *s = -1;
}

int socket_close(socket_t s) {
  if (is_socket_invalid(s)) return 0;
  return close(s);
}

net_send_file_result_t net_send_file(socket_t sock,
                                    int in_fd,
                                    uint64_t content_size) {
  if (content_size == 0)
    return NET_SEND_FILE_OK;
  if (is_socket_invalid(sock) || in_fd < 0)
    return NET_SEND_FILE_INVALID_ARGUMENT;

#if defined(__linux__)
  off_t offset = 0;
  uint64_t remaining = content_size;
  while (remaining > 0) {
    size_t want = (remaining > (uint64_t)SIZE_MAX)
                    ? (size_t)SIZE_MAX
                    : (size_t)remaining;
    ssize_t n = sendfile(sock, in_fd, &offset, want);
    if (n < 0) {
      if (errno == EINTR) continue;
      return NET_SEND_FILE_IO;
    }
    if (n == 0)
      return NET_SEND_FILE_SOURCE_CHANGED;
    remaining -= (uint64_t)n;
  }
  return NET_SEND_FILE_OK;
#elif defined(__APPLE__)
  off_t offset = 0;
  uint64_t remaining = content_size;
  while (remaining > 0) {
    off_t want = (off_t)(remaining > (uint64_t)INT64_MAX
                           ? (uint64_t)INT64_MAX
                           : remaining);
    off_t sent = want;
    int rc = sendfile(in_fd, sock, offset, &sent, NULL, 0);
    if (rc == 0) {
      if (sent <= 0)
        return NET_SEND_FILE_SOURCE_CHANGED;
      remaining -= (uint64_t)sent;
      offset += sent;
      continue;
    }
    if (sent > 0) {
      remaining -= (uint64_t)sent;
      offset += sent;
    }
    if (errno == EINTR) continue;
    if (errno == EPIPE)
      return NET_SEND_FILE_IO;
    return NET_SEND_FILE_IO;
  }
  return NET_SEND_FILE_OK;
#else
  char *buf = (char *)malloc(CHUNK_SIZE);
  if (buf == NULL)
    return NET_SEND_FILE_IO;

  uint64_t remaining = content_size;
  net_send_file_result_t result = NET_SEND_FILE_OK;
  while (remaining > 0) {
    size_t want = CHUNK_SIZE;
    if ((uint64_t)want > remaining)
      want = (size_t)remaining;

    ssize_t nr = fs_read(in_fd, buf, want);
    if (nr < 0) {
      result = NET_SEND_FILE_IO;
      break;
    }
    if (nr == 0) {
      result = NET_SEND_FILE_SOURCE_CHANGED;
      break;
    }
    if (send_all(sock, buf, (size_t)nr) != nr) {
      result = NET_SEND_FILE_IO;
      break;
    }

    remaining -= (uint64_t)nr;
  }

  free(buf);
  return result;
#endif
}

#if defined(__linux__)
static void net_close_pair(int fds[2]) {
  if (fds == NULL) {
    return;
  }
  if (fds[0] >= 0) {
    close(fds[0]);
    fds[0] = -1;
  }
  if (fds[1] >= 0) {
    close(fds[1]);
    fds[1] = -1;
  }
}
#endif

net_recv_file_result_t net_recv_file(socket_t sock,
                                      int out_fd,
                                      uint64_t content_size) {
  if (content_size == 0)
    return NET_RECV_FILE_OK;
  if (is_socket_invalid(sock) || out_fd < 0)
    return NET_RECV_FILE_INVALID_ARGUMENT;

#if defined(__linux__)
  int pipefd[2] = {-1, -1};
  uint64_t remaining = content_size;
  net_recv_file_result_t result = NET_RECV_FILE_OK;

  if (pipe(pipefd) != 0)
    return NET_RECV_FILE_IO;

  while (remaining > 0) {
    size_t want = CHUNK_SIZE;
    if ((uint64_t)want > remaining)
      want = (size_t)remaining;

    ssize_t n = splice(sock, NULL, pipefd[1], NULL, want,
                       SPLICE_F_MOVE | SPLICE_F_MORE);
    if (n < 0) {
      if (errno == EINTR) continue;
      result = NET_RECV_FILE_IO;
      goto CLEANUP;
    }
    if (n == 0) {
      result = NET_RECV_FILE_EOF;
      goto CLEANUP;
    }

    ssize_t pipe_remaining = n;
    while (pipe_remaining > 0) {
      ssize_t written = splice(pipefd[0], NULL, out_fd, NULL,
                               (size_t)pipe_remaining,
                               SPLICE_F_MOVE | SPLICE_F_MORE);
      if (written < 0) {
        if (errno == EINTR) continue;
        result = NET_RECV_FILE_IO;
        goto CLEANUP;
      }
      if (written == 0) {
        result = NET_RECV_FILE_IO;
        goto CLEANUP;
      }
      pipe_remaining -= written;
      remaining -= (uint64_t)written;
    }
  }

CLEANUP:
  net_close_pair(pipefd);
  return result;
#else
  size_t buf_cap = CHUNK_SIZE;
  char stack_buf[8192];
  char *buf = stack_buf;
  char *heap_buf = NULL;

  if (content_size <= (uint64_t)sizeof(stack_buf)) {
    buf_cap = sizeof(stack_buf);
  } else {
    heap_buf = (char *)malloc(buf_cap);
    if (heap_buf == NULL)
      return NET_RECV_FILE_IO;
    buf = heap_buf;
  }

  uint64_t remaining = content_size;
  net_recv_file_result_t result = NET_RECV_FILE_OK;
  while (remaining > 0) {
    size_t want = buf_cap;
    if ((uint64_t)want > remaining)
      want = (size_t)remaining;

    ssize_t n = recv(sock, buf, want, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      result = NET_RECV_FILE_IO;
      break;
    }
    if (n == 0) {
      result = NET_RECV_FILE_EOF;
      break;
    }

    if (fs_write_all(out_fd, buf, (size_t)n) != n) {
      result = NET_RECV_FILE_IO;
      break;
    }

    remaining -= (uint64_t)n;
  }

  free(heap_buf);
  return result;
#endif
}
