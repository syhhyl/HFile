#ifndef HF_PROTOCOL_H
#define HF_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define HF_PROTOCOL_MAX_FILE_NAME_LEN 255U
#define HF_MAX_FILE_SIZE (100ULL * 1024 * 1024 * 1024)

#define HF_PROTOCOL_HEADER_SIZE 13U
#define HF_PROTOCOL_RES_FRAME_SIZE 2U
#define HF_PROTOCOL_MAGIC 0x0429U
#define HF_PROTOCOL_VERSION 0x03U

#define HF_MSG_TYPE_SEND_FILE 0x01U
#define HF_MSG_FLAG_NONE 0x00U


typedef enum {
  PROTO_PHASE_READY = 0,
  PROTO_PHASE_FINAL
} protocol_phase_t;

typedef enum {
  PROTO_STATUS_OK = 0,
  PROTO_STATUS_REJECTED,
  PROTO_STATUS_FAILED
} protocol_status_t;

#endif  /* HF_PROTOCOL_H */
