#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <io.h>
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif


int fs_open(const char *path, int flags, int mode) {
#ifdef _WIN32
  return _open(path, flags, mode);
#else
  return open(path, flags, mode);
#endif
}

ssize_t fs_read(int fd, void *buf, size_t len) {
#ifdef _WIN32
  int n = _read(fd, buf, (unsigned)len);
  return (ssize_t)n;
#else
  return read(fd, buf, len);
#endif
}

ssize_t fs_write(int fd, const void *buf, size_t len) {
#ifdef _WIN32
  int n = _write(fd, buf, (unsigned)len);
  return (ssize_t)n;
#else
  return write(fd, buf, len);
#endif
}

int fs_close(int fd) {
#ifdef _WIN32
  return _close(fd);
#else
  return close(fd);
#endif
}


ssize_t fs_write_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  size_t total = 0;
   
  while (total < len) {
    ssize_t n = fs_write(fd, p+total, len-total);
    if (n == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return (ssize_t)total;
    total += (size_t)n;
  }
  return (ssize_t)total;
}

int fs_validate_file_name(const char *file_name) {
  if (file_name == NULL || file_name[0] == '\0') return 1;
  if (strchr(file_name, '/') != NULL) return 1;
  if (strchr(file_name, '\\') != NULL) return 1;
  if (strstr(file_name, "..") != NULL) return 1;
  return 0;
}

int fs_join_path(char *out, size_t out_cap, const char *dir, const char *file) {
  if (out == NULL || out_cap == 0) return 1;
  if (dir == NULL || file == NULL) return 1;

  const size_t dir_len = strlen(dir);

#ifdef _WIN32
  const char *sep =
    (dir_len > 0 && (dir[dir_len - 1] == '\\' || dir[dir_len - 1] == '/'))
      ? ""
      : "\\";
#else
  const char *sep = (dir_len > 0 && dir[dir_len - 1] == '/') ? "" : "/";
#endif

  int n = snprintf(out, out_cap, "%s%s%s", dir, sep, file);
  if (n < 0 || (size_t)n >= out_cap) return 1;
  return 0;
}

int fs_make_temp_path(
  char *out,
  size_t out_cap,
  const char *final_path,
  int pid,
  int attempt) {
  if (out == NULL || out_cap == 0) return 1;
  if (final_path == NULL) return 1;

  int n = snprintf(out, out_cap, "%s.tmp.%d.%d", final_path, pid, attempt);
  if (n < 0 || (size_t)n >= out_cap) return 1;
  return 0;
}

int fs_open_temp_file(const char *tmp_path) {
  if (tmp_path == NULL) {
    errno = EINVAL;
    return -1;
  }

  int flags = O_CREAT | O_WRONLY | O_TRUNC | O_EXCL;
#ifdef _WIN32
  flags |= O_BINARY;
#endif
  return fs_open(tmp_path, flags, 0644);
}

int fs_finalize_temp_file(
  const char *tmp_path,
  const char *final_path,
  unsigned long *win_err) {
  if (tmp_path == NULL || final_path == NULL) return 1;

#ifdef _WIN32
  if (!MoveFileExA(tmp_path, final_path,
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    if (win_err != NULL) *win_err = (unsigned long)GetLastError();
    return 1;
  }
  return 0;
#else
  (void)win_err;
  if (rename(tmp_path, final_path) != 0) {
    return 1;
  }
  return 0;
#endif
}

void fs_remove_quiet(const char *path) {
  if (path == NULL) return;
  (void)remove(path);
}

int fs_get_file_name(const char **file_path, const char **file_name) {
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
