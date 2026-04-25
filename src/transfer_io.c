#include "transfer_io.h"

#include "fs.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif

#define TRANSFER_CHUNK_LINE_MAX 128u

static protocol_result_t transfer_recv_socket_http_file_buffered(
  socket_t conn,
  int out,
  uint64_t content_size,
  const char *recv_ctx,
  const char *short_read_message) {
  char stack_buf[STACK_BUF_SIZE];
  char *heap_buf = NULL;
  char *buf = stack_buf;
  size_t buf_cap = STACK_BUF_SIZE;
  uint64_t remaining = content_size;

  if (content_size > HEAP_THRESHOLD) {
    heap_buf = (char *)malloc(HEAP_BUF_SIZE);
    if (heap_buf == NULL) {
      fprintf(stderr, "heap buf malloc failed\n");
      return PROTOCOL_ERR_ALLOC;
    }
    buf = heap_buf;
    buf_cap = HEAP_BUF_SIZE;
  }

  while (remaining > 0) {
    size_t want = buf_cap;
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
      free(heap_buf);
      return PROTOCOL_ERR_IO;
    }
    ssize_t n = (ssize_t)tmp;
#else
    ssize_t n = recv(conn, buf, want, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      sock_perror(recv_ctx);
      free(heap_buf);
      return PROTOCOL_ERR_IO;
    }
#endif
    if (n == 0) {
      fprintf(stderr, "%s\n", short_read_message);
      free(heap_buf);
      return PROTOCOL_ERR_EOF;
    }

    if (fs_write_all(out, buf, (size_t)n) != n) {
      perror("write_all");
      free(heap_buf);
      return PROTOCOL_ERR_IO;
    }

    remaining -= (uint64_t)n;
  }

  free(heap_buf);
  return PROTOCOL_OK;
}

static int transfer_recv_line(socket_t conn, char *out, size_t out_cap) {
  size_t len = 0;

  if (out == NULL || out_cap < 3u) {
    return 1;
  }

  while (len + 1u < out_cap) {
    char ch = '\0';
#ifdef _WIN32
    int n = recv(conn, &ch, 1, 0);
    if (n == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAEINTR) {
        continue;
      }
      return -1;
    }
#else
    ssize_t n = recv(conn, &ch, 1, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
#endif
    if (n == 0) {
      return 1;
    }

    out[len++] = ch;
    out[len] = '\0';
    if (len >= 2u && out[len - 2u] == '\r' && out[len - 1u] == '\n') {
      out[len - 2u] = '\0';
      return 0;
    }
  }

  return 2;
}

static int transfer_parse_chunk_size(const char *line, uint64_t *size_out) {
  uint64_t value = 0;
  int saw_digit = 0;

  if (line == NULL || size_out == NULL) {
    return 1;
  }

  while (*line != '\0' && *line != ';') {
    unsigned char ch = (unsigned char)*line;
    unsigned int digit = 0;

    if (!isxdigit(ch)) {
      return 1;
    }
    if (isdigit(ch)) {
      digit = (unsigned int)(ch - '0');
    } else {
      digit = 10u + (unsigned int)(tolower(ch) - 'a');
    }
    if (value > (UINT64_MAX - (uint64_t)digit) / 16u) {
      return 1;
    }
    value = value * 16u + (uint64_t)digit;
    saw_digit = 1;
    line++;
  }

  if (!saw_digit) {
    return 1;
  }

  *size_out = value;
  return 0;
}

static protocol_result_t transfer_expect_crlf(socket_t conn,
                                              const char *recv_ctx,
                                              const char *short_read_message) {
  char crlf[2];
  ssize_t n = recv_all(conn, crlf, sizeof(crlf));

  if (n < 0) {
    sock_perror(recv_ctx);
    return PROTOCOL_ERR_IO;
  }
  if ((size_t)n != sizeof(crlf)) {
    fprintf(stderr, "%s\n", short_read_message);
    return PROTOCOL_ERR_EOF;
  }
  if (crlf[0] != '\r' || crlf[1] != '\n') {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  return PROTOCOL_OK;
}

static protocol_result_t transfer_discard_chunked_trailers(
  socket_t conn,
  const char *recv_ctx,
  const char *short_read_message) {
  char line[TRANSFER_CHUNK_LINE_MAX];

  for (;;) {
    int line_res = transfer_recv_line(conn, line, sizeof(line));
    if (line_res == -1) {
      sock_perror(recv_ctx);
      return PROTOCOL_ERR_IO;
    }
    if (line_res == 1) {
      fprintf(stderr, "%s\n", short_read_message);
      return PROTOCOL_ERR_EOF;
    }
    if (line_res != 0) {
      return PROTOCOL_ERR_INVALID_ARGUMENT;
    }
    if (line[0] == '\0') {
      return PROTOCOL_OK;
    }
  }
}

static protocol_result_t transfer_recv_chunk_to_file(socket_t conn,
                                                     int out,
                                                     uint64_t chunk_size,
                                                     const char *recv_ctx,
                                                     const char *short_read_message) {
  char stack_buf[STACK_BUF_SIZE];
  char *heap_buf = NULL;
  char *buf = stack_buf;
  size_t buf_cap = STACK_BUF_SIZE;
  uint64_t remaining = chunk_size;

  if (chunk_size > HEAP_THRESHOLD) {
    heap_buf = (char *)malloc(HEAP_BUF_SIZE);
    if (heap_buf == NULL) {
      fprintf(stderr, "heap buf malloc failed\n");
      return PROTOCOL_ERR_ALLOC;
    }
    buf = heap_buf;
    buf_cap = HEAP_BUF_SIZE;
  }

  while (remaining > 0) {
    size_t want = buf_cap;
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
      free(heap_buf);
      return PROTOCOL_ERR_IO;
    }
    ssize_t n = (ssize_t)tmp;
#else
    ssize_t n = recv(conn, buf, want, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      sock_perror(recv_ctx);
      free(heap_buf);
      return PROTOCOL_ERR_IO;
    }
#endif
    if (n == 0) {
      fprintf(stderr, "%s\n", short_read_message);
      free(heap_buf);
      return PROTOCOL_ERR_EOF;
    }

    if (fs_write_all(out, buf, (size_t)n) != n) {
      perror("write_all");
      free(heap_buf);
      return PROTOCOL_ERR_IO;
    }

    remaining -= (uint64_t)n;
  }

  free(heap_buf);
  return PROTOCOL_OK;
}

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
    if (fs_make_temp_path(tmp_path, tmp_path_cap, full_path, pid, attempt) != 0) {
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
  if (out_fd == NULL || tmp_path == NULL || full_path == NULL ||
      full_path_out == NULL || full_path_cap == 0) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (*out_fd != -1 && fs_close(*out_fd) != 0) {
    perror("close(temp)");
    *out_fd = -1;
    return PROTOCOL_ERR_IO;
  }
  *out_fd = -1;

  if (fs_finalize_temp_file(tmp_path, full_path, NULL) != 0) {
    perror("rename");
    return PROTOCOL_ERR_IO;
  }

  if (strlen(full_path) + 1u > full_path_cap) {
    fprintf(stderr, "output path is too long\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  memcpy(full_path_out, full_path, strlen(full_path) + 1u);
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
    fs_remove_quiet(tmp_path);
  }
  return result;
}

protocol_result_t transfer_recv_socket_http_file(socket_t conn,
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

  result = transfer_recv_socket_http_file_buffered(
    conn, out, content_size, recv_ctx, short_read_message);
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
    fs_remove_quiet(tmp_path);
  }
  return result;
}

protocol_result_t transfer_recv_socket_chunked_file(socket_t conn,
                                                    const char *base_dir,
                                                    const char *file_name,
                                                    uint64_t max_content_size,
                                                    const char *recv_ctx,
                                                    const char *short_read_message,
                                                    char *full_path_out,
                                                    size_t full_path_cap) {
  char full_path[4096];
  char tmp_path[4096];
  char line[TRANSFER_CHUNK_LINE_MAX];
  int out = -1;
  uint64_t total = 0;
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

  for (;;) {
    uint64_t chunk_size = 0;
    int line_res = transfer_recv_line(conn, line, sizeof(line));
    if (line_res == -1) {
      sock_perror(recv_ctx);
      result = PROTOCOL_ERR_IO;
      goto CLEANUP;
    }
    if (line_res == 1) {
      fprintf(stderr, "%s\n", short_read_message);
      result = PROTOCOL_ERR_EOF;
      goto CLEANUP;
    }
    if (line_res != 0 || transfer_parse_chunk_size(line, &chunk_size) != 0) {
      result = PROTOCOL_ERR_INVALID_ARGUMENT;
      goto CLEANUP;
    }

    if (chunk_size == 0) {
      result = transfer_discard_chunked_trailers(conn, recv_ctx, short_read_message);
      if (result != PROTOCOL_OK) {
        goto CLEANUP;
      }
      break;
    }

    if (chunk_size > max_content_size || total > max_content_size - chunk_size) {
      result = PROTOCOL_ERR_MSG_TOO_LARGE;
      goto CLEANUP;
    }

    result = transfer_recv_chunk_to_file(conn, out, chunk_size, recv_ctx,
                                         short_read_message);
    if (result != PROTOCOL_OK) {
      goto CLEANUP;
    }

    result = transfer_expect_crlf(conn, recv_ctx, short_read_message);
    if (result != PROTOCOL_OK) {
      goto CLEANUP;
    }

    total += chunk_size;
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
    fs_remove_quiet(tmp_path);
  }
  return result;
}
