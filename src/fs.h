#ifndef HF_FS_H
#define HF_FS_H

#include <stdint.h>
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

#define CHUNK_SIZE 1024 * 1024

int fs_basename_from_path(const char **file_path, const char **file_name);

int fs_open(const char *path, int flags, int mode);
ssize_t fs_read(int fd, void *buf, size_t len);
ssize_t fs_write(int fd, const void *buf, size_t len);
int fs_close(int fd);
int fs_seek_start(int fd);

ssize_t fs_write_all(int fd, const void *buf, size_t len);

int fs_validate_file_name(const char *file_name);
int fs_join_path(char *out, size_t out_cap, const char *dir, const char *file);
int fs_make_temp_path(
  char *out,
  size_t out_cap,
  const char *final_path,
  int pid,
  int attempt);
int fs_open_temp_file(const char *tmp_path);
int fs_finalize_temp_file(
  const char *tmp_path,
  const char *final_path,
  unsigned long *win_err);
void fs_remove_quiet(const char *path);

#endif  // HF_FS_H
