#ifndef HF_PROTOCOL_H
#define HF_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define HF_PROTOCOL_MAX_FILE_NAME_LEN 255u
#define HF_MAX_FILE_SIZE 100ULL * 1024 * 1024 * 1024
#define HF_PROTOCOL_HEADER_SIZE 13u
#define HF_PROTOCOL_RES_FRAME_SIZE 4u

#define HF_PROTOCOL_MAGIC 0x0429u
#define HF_PROTOCOL_VERSION 0x03u

#define HF_MSG_TYPE_SEND_FILE 0x01u

#define HF_MSG_FLAG_NONE 0x00u

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
  PROTOCOL_ERR_EOF
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

#endif  /* HF_PROTOCOL_H */
