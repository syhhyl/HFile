#include "helper.h"


ssize_t write_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  size_t total = 0;
   
  while (total < len) {
#ifdef _WIN32
    int n = write(fd, p+total, (unsigned)(len-total));
    if (n == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
#else
    ssize_t n = write(fd, p+total, len-total);
    if (n == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
#endif
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  return (ssize_t)total;
}

int get_file_name(const char **file_path, const char **file_name) {
  if (*file_path == NULL) return 1;
  char *tmp;
#ifdef _WIN32
  tmp = strrchr(*file_path, '\\');
#else
  tmp = strrchr(*file_path, '/');
#endif
  if (tmp == NULL) *file_name = *file_path;
  else *file_name = tmp + 1;
  return *file_name ? 0 : 1;
}
