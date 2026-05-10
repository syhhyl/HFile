#include "transfer_io.h"

#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif

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

#ifdef _WIN32
  pid = _getpid();
#else
  pid = (int)getpid();
#endif

  for (int attempt = 0; attempt < 3; attempt++) {
    if (fs_build_temp_path(tmp_path, tmp_path_cap, full_path, pid, attempt) != 0) {
      fprintf(stderr, "temporary file path is too long\n");
      return PROTOCOL_ERR_INVALID_ARGUMENT;
    }

    *out_fd = fs_open_temp_file(tmp_path);
    if (*out_fd != -1) {
      return PROTOCOL_OK;
    }
    if (errno == EEXIST) {
      continue;
    }

    perror("open(temp)");
    return PROTOCOL_ERR_IO;
  }

  fprintf(stderr, "failed to create temporary file\n");
  return PROTOCOL_ERR_IO;
}

static protocol_result_t transfer_finalize_output(int *out_fd,
                                                  const char *tmp_path,
                                                  const char *full_path,
                                                  char *full_path_out,
                                                  size_t full_path_cap) {
  size_t full_path_len = 0;

  if (out_fd == NULL || tmp_path == NULL || full_path == NULL ||
      full_path_out == NULL || full_path_cap == 0) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  full_path_len = strlen(full_path);
  if (full_path_len + 1u > full_path_cap) {
    fprintf(stderr, "output path is too long\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (*out_fd != -1 && fs_close(*out_fd) != 0) {
    perror("close(temp)");
    *out_fd = -1;
    return PROTOCOL_ERR_IO;
  }
  *out_fd = -1;

  if (fs_commit_temp_file(tmp_path, full_path, NULL) != 0) {
    perror("rename");
    return PROTOCOL_ERR_IO;
  }

  memcpy(full_path_out, full_path, full_path_len + 1u);
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
  char full_path[4096];
  char tmp_path[4096];
  int out = -1;
  protocol_result_t result = PROTOCOL_ERR_IO;

  if (base_dir == NULL || file_name == NULL || recv_ctx == NULL ||
      short_read_message == NULL || full_path_out == NULL || full_path_cap == 0) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  tmp_path[0] = '\0';

  result = transfer_prepare_output(base_dir, file_name, full_path, sizeof(full_path),
                                   tmp_path, sizeof(tmp_path), &out);
  if (result != PROTOCOL_OK) {
    return result;
  }

  net_recv_file_result_t recv_res = net_recv_file_best_effort(conn, out, content_size);
  if (recv_res == NET_RECV_FILE_OK) {
    result = PROTOCOL_OK;
  } else if (recv_res == NET_RECV_FILE_EOF) {
    fprintf(stderr, "%s\n", short_read_message);
    result = PROTOCOL_ERR_EOF;
  } else if (recv_res == NET_RECV_FILE_IO) {
    sock_perror(recv_ctx);
    result = PROTOCOL_ERR_IO;
  } else if (recv_res == NET_RECV_FILE_INVALID_ARGUMENT) {
    result = PROTOCOL_ERR_INVALID_ARGUMENT;
  } else {
    result = PROTOCOL_ERR_IO;
  }
  if (result != PROTOCOL_OK) {
    goto CLEANUP;
  }

  result = transfer_finalize_output(&out, tmp_path, full_path, full_path_out,
                                    full_path_cap);
  if (result != PROTOCOL_OK) {
    goto CLEANUP;
  }

  result = PROTOCOL_OK;

CLEANUP:
  if (out != -1) {
    fs_close(out);
  }
  if (result != PROTOCOL_OK && tmp_path[0] != '\0') {
    fs_remove_ignore_error(tmp_path);
  }
  return result;
}
