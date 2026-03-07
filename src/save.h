#ifndef HF_SAVE_H
#define HF_SAVE_H

#include <stddef.h>

// Return 0 if file_name is acceptable, else 1.
int save_validate_file_name(const char *file_name);

// Join dir and file into out. Returns 0 on success, 1 on overflow/error.
int save_join_path(char *out, size_t out_cap, const char *dir, const char *file);

// Format a temp path like: "<final>.tmp.<pid>.<attempt>".
// Returns 0 on success, 1 on overflow/error.
int save_make_temp_path(
  char *out,
  size_t out_cap,
  const char *final_path,
  int pid,
  int attempt);

// Open a temp file for writing with O_EXCL semantics.
// Returns fd on success, -1 on error (errno is set).
int save_open_temp_file(const char *tmp_path);

// Replace final_path with tmp_path atomically (best effort on platform).
// Returns 0 on success, 1 on error.
// On Windows, sets *win_err to GetLastError() when failing.
int save_finalize_temp_file(
  const char *tmp_path,
  const char *final_path,
  unsigned long *win_err);

// Remove a path; ignore errors.
void save_remove_quiet(const char *path);

#endif  // HF_SAVE_H
