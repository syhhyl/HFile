#include "fs.h"
#include <unistd.h>


int fd_close(int fd) {
#ifdef _WIN32
  return _close(fd);
#else
  return close(fd);
#endif
}
