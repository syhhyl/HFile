#ifndef HF_APP_SERVICE_H
#define HF_APP_SERVICE_H

#include "fs.h"
#include "net.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
  APP_UPLOAD_PROTOCOL = 0,
  APP_UPLOAD_HTTP,
  APP_UPLOAD_HTTP_CHUNKED,
} app_upload_kind_t;

typedef struct {
  int fd;
  fs_path_info_t info;
} app_download_t;

protocol_result_t app_submit_message(const char *message);
protocol_result_t app_receive_file(socket_t conn,
                                   const char *base_dir,
                                   const char *target_path,
                                   uint64_t content_size,
                                   app_upload_kind_t upload_kind,
                                   char *saved_path_out,
                                   size_t saved_path_cap);
protocol_result_t app_prepare_download(const char *base_dir,
                                       const char *target_path,
                                       app_download_t *download_out);
void app_download_cleanup(app_download_t *download);

#endif  // HF_APP_SERVICE_H
