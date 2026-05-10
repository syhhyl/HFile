#ifndef HF_FS_H
#define HF_FS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

int fs_basename_from_path(const char **file_path, const char **file_name);

int fs_open(const char *path, int flags, int mode);
ssize_t fs_read(int fd, void *buf, size_t len);
ssize_t fs_write(int fd, const void *buf, size_t len);
int fs_close(int fd);

ssize_t fs_write_all(int fd, const void *buf, size_t len);

int fs_validate_file_name(const char *file_name);
int fs_validate_relative_path(const char *path);
int fs_join_path(char *out, size_t out_cap, const char *dir, const char *file);
int fs_join_relative_path(char *out, size_t out_cap, const char *base_dir,
                          const char *relative_path);
int fs_build_temp_path(
  char *out,
  size_t out_cap,
  const char *final_path,
  int pid,
  int attempt);
int fs_open_temp_file(const char *tmp_path);
int fs_commit_temp_file(
  const char *tmp_path,
  const char *final_path,
  unsigned long *win_err);
void fs_remove_ignore_error(const char *path);

#endif  // HF_FS_H
