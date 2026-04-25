#ifndef HF_PROTOCOL_H
#define HF_PROTOCOL_H

#include "net.h"

#include <stdint.h>
#include <stddef.h>

#define HF_PROTOCOL_MAX_FILE_NAME_LEN 255u
#define HF_MAX_FILE_SIZE 100ULL * 1024 * 1024 * 1024
#define HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE (256u * 1024u)
#define HF_PROTOCOL_HEADER_SIZE 13u
#define HF_PROTOCOL_RES_FRAME_SIZE 4u

#define HF_PROTOCOL_MAGIC 0x0429u
#define HF_PROTOCOL_VERSION 0x03u

#define HF_MSG_TYPE_SEND_FILE 0x01u
#define HF_MSG_TYPE_TEXT_MESSAGE 0x02u
#define HF_MSG_TYPE_GET_FILE 0x03u

#define HF_MSG_FLAG_NONE 0x00u

// protocol header struct
typedef struct {
  uint16_t magic; 
  uint8_t version;
  uint8_t msg_type;
  uint8_t flags;
  uint64_t payload_size;
} protocol_header_t;


typedef enum {
  PROTOCOL_OK = 0,
  PROTOCOL_ERR_HEADER_MAGIC,
  PROTOCOL_ERR_HEADER_VERSION,
  PROTOCOL_ERR_HEADER_MSG_TYPE,
  PROTOCOL_ERR_HEADER_MSG_FLAG,
  PROTOCOL_ERR_INVALID_ARGUMENT,
  PROTOCOL_ERR_FILE_NAME_LEN,
  PROTOCOL_ERR_INVALID_FILE_NAME,
  PROTOCOL_ERR_PAYLOAD_SIZE_MISMATCH,
  PROTOCOL_ERR_IO,
  PROTOCOL_ERR_SHORT_WRITE,
  PROTOCOL_ERR_ALLOC,
  PROTOCOL_ERR_EOF,
  PROTOCOL_ERR_MSG_TOO_LARGE
} protocol_result_t;

typedef enum {
  PROTO_PHASE_READY = 0,
  PROTO_PHASE_FINAL
} protocol_phase_t;

typedef enum {
  PROTO_STATUS_OK = 0,
  PROTO_STATUS_REJECTED,
  PROTO_STATUS_FAILED
} protocol_status_t;

typedef struct {
  uint8_t phase;
  uint8_t status;
  uint16_t error_code;
} res_frame_t;


void init_header(protocol_header_t *header);

protocol_result_t encode_header(const protocol_header_t *header, uint8_t *out);
protocol_result_t decode_header(protocol_header_t *header, const uint8_t *in);
protocol_result_t recv_header(socket_t sock, uint8_t *out);

protocol_result_t encode_res_frame(const res_frame_t *frame, uint8_t *out);
protocol_result_t decode_res_frame(res_frame_t *frame, const uint8_t *in);
protocol_result_t send_res_frame(socket_t sock, const res_frame_t *frame);
protocol_result_t recv_res_frame(socket_t sock, res_frame_t *frame);



int proto_get_file_name_len(const char *file_name, uint16_t *out_len);
size_t proto_file_name_only_size(uint16_t file_name_len);
size_t proto_file_transfer_prefix_size(uint16_t file_name_len);
protocol_result_t encode_file_name_only(const char *file_name, uint8_t *out);
protocol_result_t encode_file_prefix(const char *file_name,
                                     uint64_t content_size,
                                     uint8_t *out);
protocol_result_t proto_send_payload(socket_t sock,
                                     const uint8_t *in,
                                     size_t len);
protocol_result_t proto_recv_file_name_only(socket_t sock,
                                            char **file_name_out);
protocol_result_t proto_recv_file_transfer_prefix(socket_t sock,
                                                       char **file_name_out,
                                                       uint64_t *content_size_out);

#endif  // HF_PROTOCOL_H
