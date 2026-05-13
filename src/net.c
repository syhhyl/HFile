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

#define NET_MIN_TRANSFER_TIMEOUT_MS 60000u
#define NET_MAX_TRANSFER_TIMEOUT_MS 300000u
#define NET_TIMEOUT_BYTES_PER_MS (10u * 1024u)

#if defined(__linux__)
  #include <sys/sendfile.h>
#elif defined(__APPLE__)
  #include <sys/socket.h>
  #include <sys/uio.h>
#endif


bool is_socket_invalid(socket_t sock) {
  if (sock < 0) return true;
  return false;
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

uint32_t net_transfer_timeout_ms(uint64_t content_size) {
  uint64_t scaled = content_size / NET_TIMEOUT_BYTES_PER_MS;

  if (scaled < NET_MIN_TRANSFER_TIMEOUT_MS) {
    return NET_MIN_TRANSFER_TIMEOUT_MS;
  }
  if (scaled > NET_MAX_TRANSFER_TIMEOUT_MS) {
    return NET_MAX_TRANSFER_TIMEOUT_MS;
  }
  return (uint32_t)scaled;
}

int net_set_recv_timeout(socket_t sock, uint32_t timeout_ms) {
  struct timeval tv;

  if (is_socket_invalid(sock)) {
    return 1;
  }

  tv.tv_sec = (time_t)(timeout_ms / 1000u);
  tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
  return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ? 1 : 0;
}

int net_set_send_timeout(socket_t sock, uint32_t timeout_ms) {
  struct timeval tv;

  if (is_socket_invalid(sock)) {
    return 1;
  }

  tv.tv_sec = (time_t)(timeout_ms / 1000u);
  tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
  return setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0 ? 1 : 0;
}

int net_set_socket_timeouts(socket_t sock, uint32_t timeout_ms) {
  if (net_set_recv_timeout(sock, timeout_ms) != 0) {
    return 1;
  }
  return net_set_send_timeout(sock, timeout_ms);
}

int net_wait_readable(socket_t sock, uint32_t timeout_ms, int *ready_out) {
  if (ready_out == NULL) {
    return 1;
  }

  *ready_out = 0;

  if (is_socket_invalid(sock)) {
    return 1;
  }

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(sock, &readfds);

  struct timeval tv;
  tv.tv_sec = (long)(timeout_ms / 1000u);
  tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

  int rc = select(sock + 1, &readfds, NULL, NULL, &tv);
  if (rc < 0) {
    if (errno == EINTR) {
      return 0;
    }
    return 1;
  }

  if (rc == 0) {
    return 0;
  }

  *ready_out = FD_ISSET(sock, &readfds) ? 1 : 0;
  return 0;
}

static net_send_file_result_t net_send_file_all(socket_t sock,
                                                int in_fd,
                                                uint64_t content_size) {
  if (content_size == 0) {
    return NET_SEND_FILE_OK;
  }

  if (is_socket_invalid(sock) || in_fd < 0) {
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

      if (errno == EINTR) {
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
}

static net_send_file_result_t net_send_file_buffered(socket_t sock,
                                                     int in_fd,
                                                     uint64_t content_size) {
  int exit_code = NET_SEND_FILE_OK;
  char *buf = NULL;
  uint64_t remaining = content_size;

  if (is_socket_invalid(sock) || in_fd < 0) {
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

static net_recv_file_result_t net_recv_file_all(socket_t sock,
                                                int out_fd,
                                                uint64_t content_size) {
  if (content_size == 0) {
    return NET_RECV_FILE_OK;
  }

#if defined(__linux__)
  if (is_socket_invalid(sock) || out_fd < 0) {
    return NET_RECV_FILE_INVALID_ARGUMENT;
  }

  int pipefd[2] = {-1, -1};
  uint64_t remaining = content_size;
  uint64_t moved = 0;
  net_recv_file_result_t result = NET_RECV_FILE_OK;

  if (pipe(pipefd) != 0) {
    if (errno == EMFILE || errno == ENFILE) {
      return NET_RECV_FILE_IO;
    }
    return NET_RECV_FILE_UNSUPPORTED;
  }

  while (remaining > 0) {
    size_t want = CHUNK_SIZE;
    if ((uint64_t)want > remaining) {
      want = (size_t)remaining;
    }

    ssize_t n = splice(sock, NULL, pipefd[1], NULL, want,
                       SPLICE_F_MOVE | SPLICE_F_MORE);
    if (n < 0) {
      if (errno == EINVAL || errno == ENOSYS || errno == ENOTSUP) {
        result = moved == 0 ? NET_RECV_FILE_UNSUPPORTED : NET_RECV_FILE_IO;
        goto CLEANUP;
      }
      if (errno == EINTR) {
        continue;
      }
      result = NET_RECV_FILE_IO;
      goto CLEANUP;
    }
    if (n == 0) {
      result = NET_RECV_FILE_EOF;
      goto CLEANUP;
    }

    ssize_t pipe_remaining = n;
    while (pipe_remaining > 0) {
      ssize_t written = splice(pipefd[0], NULL, out_fd, NULL, (size_t)pipe_remaining,
                               SPLICE_F_MOVE | SPLICE_F_MORE);
      if (written < 0) {
        if (errno == EINVAL || errno == ENOSYS || errno == ENOTSUP) {
          result = NET_RECV_FILE_IO;
          goto CLEANUP;
        }
        if (errno == EINTR) {
          continue;
        }
        result = NET_RECV_FILE_IO;
        goto CLEANUP;
      }
      if (written == 0) {
        result = NET_RECV_FILE_IO;
        goto CLEANUP;
      }
      pipe_remaining -= written;
      remaining -= (uint64_t)written;
      moved += (uint64_t)written;
    }
  }

CLEANUP:
  net_close_pair(pipefd);
  return result;
#else
  (void)sock;
  (void)out_fd;
  (void)content_size;
  return NET_RECV_FILE_UNSUPPORTED;
#endif
}

static net_recv_file_result_t net_recv_file_buffered(socket_t sock,
                                                     int out_fd,
                                                     uint64_t content_size) {
  char stack_buf[8192];
  char *buf = stack_buf;
  size_t buf_cap = sizeof(stack_buf);
  char *heap_buf = NULL;
  uint64_t remaining = content_size;

  if (is_socket_invalid(sock) || out_fd < 0) {
    return NET_RECV_FILE_INVALID_ARGUMENT;
  }

  if (content_size == 0) {
    return NET_RECV_FILE_OK;
  }

  if (content_size > (uint64_t)(1024u * 1024u)) {
    buf_cap = 256u * 1024u;
    heap_buf = (char *)malloc(buf_cap);
    if (heap_buf == NULL) {
      return NET_RECV_FILE_IO;
    }
    buf = heap_buf;
  }

  while (remaining > 0) {
    size_t want = buf_cap;
    if ((uint64_t)want > remaining) {
      want = (size_t)remaining;
    }

    ssize_t n = recv(sock, buf, want, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(heap_buf);
      return NET_RECV_FILE_IO;
    }
    if (n == 0) {
      free(heap_buf);
      return NET_RECV_FILE_EOF;
    }

    if (fs_write_all(out_fd, buf, (size_t)n) != n) {
      free(heap_buf);
      return NET_RECV_FILE_IO;
    }

    remaining -= (uint64_t)n;
  }

  free(heap_buf);
  return NET_RECV_FILE_OK;
}

net_recv_file_result_t net_recv_file_best_effort(socket_t sock,
                                                  int out_fd,
                                                  uint64_t content_size) {
  net_recv_file_result_t res = net_recv_file_all(sock, out_fd, content_size);
  if (res != NET_RECV_FILE_UNSUPPORTED) {
    return res;
  }

  return net_recv_file_buffered(sock, out_fd, content_size);
}
