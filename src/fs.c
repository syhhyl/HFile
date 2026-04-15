#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #include <windows.h>
#else
  #include <dirent.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#ifdef _WIN32
static uint64_t fs_filetime_to_unix_seconds(FILETIME filetime) {
  ULARGE_INTEGER value;
  value.LowPart = filetime.dwLowDateTime;
  value.HighPart = filetime.dwHighDateTime;

  if (value.QuadPart < 116444736000000000ULL) {
    return 0;
  }
  return (value.QuadPart - 116444736000000000ULL) / 10000000ULL;
}
#endif

static int fs_remove_tree_validate(const char *path);
static int fs_remove_tree_impl(const char *path);


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

int fs_seek_start(int fd) {
#ifdef _WIN32
  return _lseeki64(fd, 0, SEEK_SET) < 0 ? -1 : 0;
#else
  return lseek(fd, 0, SEEK_SET) < 0 ? -1 : 0;
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

int fs_validate_relative_path(const char *path) {
  const char *segment = NULL;

  if (path == NULL || path[0] == '\0') return 1;
  if (path[0] == '/') return 1;
  if (strchr(path, '\\') != NULL) return 1;

  segment = path;
  while (*segment != '\0') {
    const char *slash = strchr(segment, '/');
    size_t len = slash != NULL ? (size_t)(slash - segment) : strlen(segment);
    if (len == 0) return 1;
    if (len == 1 && segment[0] == '.') return 1;
    if (len == 2 && segment[0] == '.' && segment[1] == '.') return 1;
    if (slash == NULL) {
      break;
    }
    segment = slash + 1;
  }

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

int fs_join_relative_path(char *out, size_t out_cap, const char *base_dir,
                          const char *relative_path) {
  if (out == NULL || out_cap == 0) return 1;
  if (base_dir == NULL) return 1;
  if (relative_path == NULL || relative_path[0] == '\0') {
    int n = snprintf(out, out_cap, "%s", base_dir);
    return n < 0 || (size_t)n >= out_cap ? 1 : 0;
  }
  if (fs_validate_relative_path(relative_path) != 0) return 1;
  if (fs_join_path(out, out_cap, base_dir, relative_path) != 0) return 1;

#ifdef _WIN32
  for (char *p = out; *p != '\0'; p++) {
    if (*p == '/') {
      *p = '\\';
    }
  }
#endif
  return 0;
}

int fs_get_path_info(const char *path, fs_path_info_t *out) {
  if (path == NULL || out == NULL) {
    return 1;
  }

#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA data;
  ULONGLONG file_size = 0;

  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
    return 1;
  }

  out->mtime = fs_filetime_to_unix_seconds(data.ftLastWriteTime);
  out->size = 0;

  if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    out->kind = FS_PATH_KIND_SYMLINK;
    return 0;
  }
  if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    out->kind = FS_PATH_KIND_DIR;
    return 0;
  }

  file_size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
  out->kind = FS_PATH_KIND_FILE;
  out->size = (uint64_t)file_size;
  return 0;
#else
  struct stat st;

  if (lstat(path, &st) != 0) {
    return 1;
  }

  out->mtime = (uint64_t)st.st_mtime;
  out->size = 0;

  if (S_ISLNK(st.st_mode)) {
    out->kind = FS_PATH_KIND_SYMLINK;
    return 0;
  }
  if (S_ISDIR(st.st_mode)) {
    out->kind = FS_PATH_KIND_DIR;
    return 0;
  }
  if (S_ISREG(st.st_mode)) {
    if (st.st_size < 0) {
      return 1;
    }
    out->kind = FS_PATH_KIND_FILE;
    out->size = (uint64_t)st.st_size;
    return 0;
  }

  out->kind = FS_PATH_KIND_OTHER;
  return 0;
#endif
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

int fs_remove_tree(const char *path) {
  if (path == NULL || path[0] == '\0') {
    errno = EINVAL;
    return 1;
  }
  if (fs_remove_tree_validate(path) != 0) {
    return 1;
  }
  return fs_remove_tree_impl(path);
}

void fs_remove_quiet(const char *path) {
  if (path == NULL) return;
  (void)remove(path);
}

int fs_basename_from_path(const char **file_path, const char **file_name) {
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

static int fs_remove_tree_validate(const char *path) {
  fs_path_info_t info = {0};

  if (fs_get_path_info(path, &info) != 0) {
    return 1;
  }
  if (info.kind == FS_PATH_KIND_FILE) {
    return 0;
  }
  if (info.kind != FS_PATH_KIND_DIR) {
    errno = EINVAL;
    return 1;
  }

#ifdef _WIN32
  char pattern[4096];
  WIN32_FIND_DATAA find_data;
  HANDLE handle = INVALID_HANDLE_VALUE;

  if (fs_join_path(pattern, sizeof(pattern), path, "*") != 0) {
    errno = ENAMETOOLONG;
    return 1;
  }

  handle = FindFirstFileA(pattern, &find_data);
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND) {
      return 0;
    }
    errno = EIO;
    return 1;
  }

  do {
    const char *name = find_data.cFileName;
    char child_path[4096];

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    if (fs_join_path(child_path, sizeof(child_path), path, name) != 0) {
      FindClose(handle);
      errno = ENAMETOOLONG;
      return 1;
    }
    if (fs_remove_tree_validate(child_path) != 0) {
      FindClose(handle);
      return 1;
    }
  } while (FindNextFileA(handle, &find_data) != 0);

  if (GetLastError() != ERROR_NO_MORE_FILES) {
    FindClose(handle);
    errno = EIO;
    return 1;
  }

  FindClose(handle);
  return 0;
#else
  DIR *dp = opendir(path);
  struct dirent *de = NULL;

  if (dp == NULL) {
    return 1;
  }

  while ((de = readdir(dp)) != NULL) {
    char child_path[4096];

    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    if (fs_join_path(child_path, sizeof(child_path), path, de->d_name) != 0) {
      closedir(dp);
      errno = ENAMETOOLONG;
      return 1;
    }
    if (fs_remove_tree_validate(child_path) != 0) {
      closedir(dp);
      return 1;
    }
  }

  closedir(dp);
  return 0;
#endif
}

static int fs_remove_tree_impl(const char *path) {
  fs_path_info_t info = {0};

  if (fs_get_path_info(path, &info) != 0) {
    return 1;
  }
  if (info.kind == FS_PATH_KIND_FILE) {
    return remove(path) == 0 ? 0 : 1;
  }
  if (info.kind != FS_PATH_KIND_DIR) {
    errno = EINVAL;
    return 1;
  }

#ifdef _WIN32
  char pattern[4096];
  WIN32_FIND_DATAA find_data;
  HANDLE handle = INVALID_HANDLE_VALUE;

  if (fs_join_path(pattern, sizeof(pattern), path, "*") != 0) {
    errno = ENAMETOOLONG;
    return 1;
  }

  handle = FindFirstFileA(pattern, &find_data);
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    if (err != ERROR_FILE_NOT_FOUND) {
      errno = EIO;
      return 1;
    }
  } else {
    do {
      const char *name = find_data.cFileName;
      char child_path[4096];

      if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        continue;
      }
      if (fs_join_path(child_path, sizeof(child_path), path, name) != 0) {
        FindClose(handle);
        errno = ENAMETOOLONG;
        return 1;
      }
      if (fs_remove_tree_impl(child_path) != 0) {
        FindClose(handle);
        return 1;
      }
    } while (FindNextFileA(handle, &find_data) != 0);

    if (GetLastError() != ERROR_NO_MORE_FILES) {
      FindClose(handle);
      errno = EIO;
      return 1;
    }
    FindClose(handle);
  }

  return RemoveDirectoryA(path) ? 0 : 1;
#else
  DIR *dp = opendir(path);
  struct dirent *de = NULL;

  if (dp == NULL) {
    return 1;
  }

  while ((de = readdir(dp)) != NULL) {
    char child_path[4096];

    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    if (fs_join_path(child_path, sizeof(child_path), path, de->d_name) != 0) {
      closedir(dp);
      errno = ENAMETOOLONG;
      return 1;
    }
    if (fs_remove_tree_impl(child_path) != 0) {
      closedir(dp);
      return 1;
    }
  }

  if (closedir(dp) != 0) {
    return 1;
  }

  return rmdir(path) == 0 ? 0 : 1;
#endif
}
