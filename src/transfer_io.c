#include "transfer_io.h"

#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif

int transfer_recv_socket_file(socket_t conn,
                              const char *base_dir,
                              const char *file_name,
                              uint64_t content_size,
                              const char *recv_ctx,
                              const char *short_read_message,
                              char *full_path_out,
                              size_t full_path_cap) {
  char full_path[4096];
  char tmp_path[4096];
  char buf[8192];
  int out = -1;
  int write_failed = 0;
  int exit_code = 1;
  uint64_t remaining = content_size;

  if (base_dir == NULL || file_name == NULL || recv_ctx == NULL ||
      short_read_message == NULL || full_path_out == NULL || full_path_cap == 0) {
    return 1;
  }

  tmp_path[0] = '\0';

  if (fs_join_path(full_path, sizeof(full_path), base_dir, file_name) != 0) {
    fprintf(stderr, "output path is too long\n");
    return 1;
  }

#ifdef _WIN32
  int pid = _getpid();
#else
  int pid = (int)getpid();
#endif

  for (int attempt = 0; attempt < 16; attempt++) {
    if (fs_make_temp_path(tmp_path, sizeof(tmp_path), full_path, pid, attempt) != 0) {
      fprintf(stderr, "temporary file path is too long\n");
      return 1;
    }

    out = fs_open_temp_file(tmp_path);
    if (out != -1) {
      break;
    }
    if (errno == EEXIST) {
      continue;
    }

    perror("open(temp)");
    return 1;
  }

  if (out == -1) {
    fprintf(stderr, "failed to create temporary file\n");
    return 1;
  }

  while (remaining > 0) {
    size_t want = sizeof(buf);
    if ((uint64_t)want > remaining) {
      want = (size_t)remaining;
    }

#ifdef _WIN32
    int tmp = recv(conn, buf, (int)want, 0);
    if (tmp == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAEINTR) {
        continue;
      }
      sock_perror(recv_ctx);
      goto CLEANUP;
    }
    ssize_t n = (ssize_t)tmp;
#else
    ssize_t n = recv(conn, buf, want, 0);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      sock_perror(recv_ctx);
      goto CLEANUP;
    }
#endif
    if (n == 0) {
      fprintf(stderr, "%s\n", short_read_message);
      goto CLEANUP;
    }

    if (!write_failed && fs_write_all(out, buf, (size_t)n) != n) {
      perror("write_all");
      write_failed = 1;
      if (fs_close(out) != 0) {
        perror("close(temp)");
      }
      out = -1;
    }

    remaining -= (uint64_t)n;
  }

  if (write_failed) {
    goto CLEANUP;
  }

  if (fs_close(out) != 0) {
    perror("close(temp)");
    out = -1;
    goto CLEANUP;
  }
  out = -1;

  if (fs_finalize_temp_file(tmp_path, full_path, NULL) != 0) {
    perror("rename");
    goto CLEANUP;
  }

  if (strlen(full_path) + 1u > full_path_cap) {
    fprintf(stderr, "output path is too long\n");
    goto CLEANUP;
  }

  memcpy(full_path_out, full_path, strlen(full_path) + 1u);
  exit_code = 0;

CLEANUP:
  if (out != -1) {
    fs_close(out);
  }
  if (exit_code != 0 && tmp_path[0] != '\0') {
    fs_remove_quiet(tmp_path);
  }
  return exit_code;
}
