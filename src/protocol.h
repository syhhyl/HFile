#ifndef HF_PROTOCOL_H
#define HF_PROTOCOL_H

#include "net.h"

#include <stdint.h>
#include <stddef.h>

#define HF_PROTOCOL_MAX_FILE_NAME_LEN 255u

typedef enum {
  PROTOCOL_OK = 0,
  PROTOCOL_ERR_INVALID_ARGUMENT,
  PROTOCOL_ERR_FILE_NAME_LEN,
  PROTOCOL_ERR_IO,
  PROTOCOL_ERR_ALLOC,
  PROTOCOL_ERR_EOF
} protocol_result_t;

int protocol_get_file_name_len(const char *file_name, uint16_t *out_len);
size_t protocol_header_size(uint16_t file_name_len);
protocol_result_t protocol_send_header(socket_t sock, const char *file_name,
                                       uint64_t content_size);
protocol_result_t protocol_recv_header(socket_t sock, char **file_name_out,
                                       uint64_t *content_size_out);

#endif  // HF_PROTOCOL_H
