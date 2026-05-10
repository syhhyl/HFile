#include "transfer_io.h"

#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static protocol_result_t transfer_prepare_output(const char *base_dir,
                                                 const char *file_name,
                                                 char *full_path,
                                                 size_t full_path_cap,
                                                 char *tmp_path,
                                                 size_t tmp_path_cap,
                                                 int *out_fd) {
  int pid = 0;

  if (base_dir == NULL || file_name == NULL || full_path == NULL ||
      full_path_cap == 0 || tmp_path == NULL || tmp_path_cap == 0 ||
      out_fd == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (fs_join_relative_path(full_path, full_path_cap, base_dir, file_name) != 0) {
    fprintf(stderr, "output path is too long\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  pid = (int)getpid();

  for (int attempt = 0; attempt < 3; attempt++) {
    if (fs_build_temp_path(tmp_path, tmp_path_cap, full_path, pid, attempt) != 0) {
      continue;
    }

    int fd = fs_open_temp_file(tmp_path);
    if (fd >= 0) {
      *out_fd = fd;
      return PROTOCOL_OK;
    }
    if (fd < 0 && errno != EEXIST) {
      perror("open temp file");
      return PROTOCOL_ERR_IO;
    }
  }

  perror("open temp file");
  return PROTOCOL_ERR_IO;
}

static protocol_result_t transfer_finalize_output(int fd,
                                                  const char *tmp_path,
                                                  const char *full_path) {
  protocol_result_t result = PROTOCOL_OK;

  if (fd < 0 || tmp_path == NULL || full_path == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (fs_close(fd) != 0) {
    perror("close temp file");
    result = PROTOCOL_ERR_IO;
  }

  if (result != PROTOCOL_OK) {
    fs_remove_ignore_error(tmp_path);
    return result;
  }

  if (fs_commit_temp_file(tmp_path, full_path, NULL) != 0) {
    perror("finalize temp file");
    fs_remove_ignore_error(tmp_path);
    return PROTOCOL_ERR_IO;
  }

  return PROTOCOL_OK;
}

protocol_result_t transfer_recv_socket_file(socket_t conn,
                                            const char *base_dir,
                                            const char *file_name,
                                            uint64_t content_size,
                                            const char *recv_ctx,
                                            const char *short_read_message,
                                            char *full_path_out,
                                            size_t full_path_cap) {
  int fd = -1;
  protocol_result_t result = PROTOCOL_ERR_IO;
  char full_path[4096];
  char tmp_path[4096];

  if (base_dir == NULL || file_name == NULL) {
    fprintf(stderr, "invalid receive file args\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  result = transfer_prepare_output(base_dir, file_name,
                                   full_path, sizeof(full_path),
                                   tmp_path, sizeof(tmp_path), &fd);
  if (result != PROTOCOL_OK) {
    return result;
  }

  net_recv_file_result_t recv_res = net_recv_file_best_effort(conn, fd, content_size);
  if (recv_res == NET_RECV_FILE_OK) {
    result = transfer_finalize_output(fd, tmp_path, full_path);
    if (result == PROTOCOL_OK && full_path_out != NULL && full_path_cap > 0) {
      strncpy(full_path_out, full_path, full_path_cap - 1);
      full_path_out[full_path_cap - 1] = '\0';
    }
  } else {
    if (recv_res == NET_RECV_FILE_EOF) {
      if (short_read_message != NULL) {
        fprintf(stderr, "%s\n", short_read_message);
      }
      result = PROTOCOL_ERR_EOF;
    } else if (recv_res == NET_RECV_FILE_INVALID_ARGUMENT) {
      fprintf(stderr, "invalid receive file arguments\n");
      result = PROTOCOL_ERR_INVALID_ARGUMENT;
    } else {
      sock_perror(recv_ctx);
      result = PROTOCOL_ERR_IO;
    }

    fs_close(fd);
    fs_remove_ignore_error(tmp_path);
  }

  return result;
}
