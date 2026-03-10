#ifndef HF_PROTOCOL_H
#define HF_PROTOCOL_H

#include "net.h"

#include <stdint.h>
#include <stddef.h>

#define HF_PROTOCOL_MAX_FILE_NAME_LEN 255u
#define HF_PROTOCOL_HEADER_SIZE 13u

// protocol header struct
typedef struct {
  uint16_t magic; 
  uint8_t version;
  uint8_t msg_type;
  uint8_t flags;
  uint64_t payload_size;
} protocol_header;


typedef enum {
  PROTOCOL_OK = 0,
  PROTOCOL_ERR_INVALID_ARGUMENT,
  PROTOCOL_ERR_FILE_NAME_LEN,
  PROTOCOL_ERR_IO,
  PROTOCOL_ERR_ALLOC,
  PROTOCOL_ERR_EOF
} protocol_result_t;


protocol_result_t encode_header(const protocol_header *header, uint8_t *out);
protocol_result_t decode_header(protocol_header *header, const uint8_t *in);
protocol_result_t send_header(socket_t sock, const uint8_t *in);
protocol_result_t recv_header(socket_t sock, uint8_t *out);



int protocol_get_file_name_len(const char *file_name, uint16_t *out_len);
size_t protocol_header_size(uint16_t file_name_len);
protocol_result_t protocol_send_header(socket_t sock, const char *file_name,
                                       uint64_t content_size);
protocol_result_t protocol_recv_header(socket_t sock, char **file_name_out,
                                       uint64_t *content_size_out);

#endif  // HF_PROTOCOL_H
