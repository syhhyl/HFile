#include "save.h"

#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#endif

int save_validate_file_name(const char *file_name) {
  if (file_name == NULL || file_name[0] == '\0') return 1;
  if (strchr(file_name, '/') != NULL) return 1;
  if (strchr(file_name, '\\') != NULL) return 1;
  if (strstr(file_name, "..") != NULL) return 1;
  return 0;
}

int save_join_path(char *out, size_t out_cap, const char *dir, const char *file) {
  if (out == NULL || out_cap == 0) return 1;
  if (dir == NULL || file == NULL) return 1;

  const size_t dir_len = strlen(dir);

#ifdef _WIN32
  const char *sep = (dir_len > 0 && (dir[dir_len - 1] == '\\' || dir[dir_len - 1] == '/'))
                         ? ""
                         : "\\";
#else
  const char *sep = (dir_len > 0 && dir[dir_len - 1] == '/') ? "" : "/";
#endif

  int n = snprintf(out, out_cap, "%s%s%s", dir, sep, file);
  if (n < 0 || (size_t)n >= out_cap) return 1;
  return 0;
}

int save_make_temp_path(
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

int save_open_temp_file(const char *tmp_path) {
  if (tmp_path == NULL) {
    errno = EINVAL;
    return -1;
  }

  int flags = O_CREAT | O_WRONLY | O_TRUNC | O_EXCL;
#ifdef _WIN32
  flags |= O_BINARY;
#endif
  return hf_open(tmp_path, flags, 0644);
}

int save_finalize_temp_file(
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

void save_remove_quiet(const char *path) {
  if (path == NULL) return;
  (void)remove(path);
}
