#ifndef HF_FS_H
#define HF_FS_H

#include <stddef.h>

#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>

  // ssize_t is POSIX; provide it on Windows toolchains that lack it.
  #if defined(__MINGW32__) || defined(__MINGW64__)
    #include <sys/types.h>
  #else
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
  #endif

  #ifndef O_BINARY
    #ifdef _O_BINARY
      #define O_BINARY _O_BINARY
    #else
      #define O_BINARY 0
    #endif
  #endif

#else
  #include <sys/types.h>
#endif

int hf_open(const char *path, int flags, int mode);
ssize_t hf_read(int fd, void *buf, size_t len);
ssize_t hf_write(int fd, const void *buf, size_t len);
int hf_close(int fd);

ssize_t write_all(int fd, const void *buf, size_t len);

#endif  // HF_FS_H
