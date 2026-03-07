#include "fs.h"
#include <errno.h>

#ifdef _WIN32
  #include <io.h>
#else
  #include <unistd.h>
  #include <fcntl.h>
#endif

int hf_open(const char *path, int flags, int mode) {
#ifdef _WIN32
  return _open(path, flags, mode);
#else
  return open(path, flags, mode);
#endif
}

ssize_t hf_read(int fd, void *buf, size_t len) {
#ifdef _WIN32
  int n = _read(fd, buf, (unsigned)len);
  return (ssize_t)n;
#else
  return read(fd, buf, len);
#endif
}

ssize_t hf_write(int fd, const void *buf, size_t len) {
#ifdef _WIN32
  int n = _write(fd, buf, (unsigned)len);
  return (ssize_t)n;
#else
  return write(fd, buf, len);
#endif
}

int hf_close(int fd) {
#ifdef _WIN32
  return _close(fd);
#else
  return close(fd);
#endif
}


ssize_t write_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  size_t total = 0;
   
  while (total < len) {
    ssize_t n = hf_write(fd, p+total, len-total);
    if (n == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  return (ssize_t)total;
}