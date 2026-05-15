#include "fs.h"
#include <unistd.h>

int fs_close(int fd) {
  return close(fd);
}
