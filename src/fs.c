#include "fs.h"

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

// int fd_close(int fd) {
//   return hf_close(fd);
// }
