#include "app_service.h"

#include "message_store.h"
#include "transfer_io.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

protocol_result_t app_submit_message(const char *message) {
  if (message == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (strlen(message) > HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE) {
    return PROTOCOL_ERR_MSG_TOO_LARGE;
  }

  if (message_store_set(message) != 0) {
    fprintf(stderr, "failed to store latest message\n");
    return PROTOCOL_ERR_IO;
  }

  return PROTOCOL_OK;
}

protocol_result_t app_receive_file(socket_t conn,
                                   const char *base_dir,
                                   const char *target_path,
                                   uint64_t content_size,
                                   app_upload_kind_t upload_kind,
                                   char *saved_path_out,
                                   size_t saved_path_cap) {
  if (base_dir == NULL || target_path == NULL || saved_path_out == NULL ||
      saved_path_cap == 0) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  switch (upload_kind) {
    case APP_UPLOAD_PROTOCOL:
      return transfer_recv_socket_file(conn, base_dir, target_path, content_size,
                                       "recv(file_body)",
                                       "protocol error: unexpected EOF while receiving file",
                                       saved_path_out, saved_path_cap);

    case APP_UPLOAD_HTTP:
      return transfer_recv_socket_http_file(conn, base_dir, target_path,
                                            content_size, "recv(http_body)",
                                            "http upload ended early",
                                            saved_path_out, saved_path_cap);
  }

  return PROTOCOL_ERR_INVALID_ARGUMENT;
}

protocol_result_t app_prepare_download(const char *base_dir,
                                       const char *target_path,
                                       app_download_t *download_out) {
  char full_path[4096];
  int fd = -1;
  int open_flags = O_RDONLY;
#ifdef _WIN32
  struct _stat64 st;
#else
  struct stat st;
#endif

  if (base_dir == NULL || target_path == NULL || download_out == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  if (fs_join_relative_path(full_path, sizeof(full_path), base_dir, target_path) != 0) {
    fprintf(stderr, "output path is too long\n");
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

#ifdef _WIN32
  open_flags |= O_BINARY;
#else
  #ifdef O_NOFOLLOW
  open_flags |= O_NOFOLLOW;
  #endif
#endif
  fd = fs_open(full_path, open_flags, 0);
  if (fd == -1) {
    return PROTOCOL_ERR_IO;
  }

#ifdef _WIN32
  if (_fstat64(fd, &st) != 0) {
#else
  if (fstat(fd, &st) != 0) {
#endif
    fs_close(fd);
    return PROTOCOL_ERR_IO;
  }

#ifdef _WIN32
  if ((st.st_mode & _S_IFMT) != _S_IFREG || st.st_size < 0) {
#else
  if (!S_ISREG(st.st_mode) || st.st_size < 0) {
#endif
    fs_close(fd);
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  download_out->fd = fd;
  download_out->info.kind = FS_PATH_KIND_FILE;
  download_out->info.size = (uint64_t)st.st_size;
  download_out->info.mtime = (uint64_t)st.st_mtime;
  return PROTOCOL_OK;
}

void app_download_cleanup(app_download_t *download) {
  if (download == NULL) {
    return;
  }

  if (download->fd != -1) {
    fs_close(download->fd);
    download->fd = -1;
  }
}
