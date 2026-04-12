#include "protocol.h"
#include "net.h"

#include <stdlib.h>
#include <string.h>

static int proto_res_frame_phase_valid(uint8_t phase) {
  return phase == PROTO_PHASE_READY || phase == PROTO_PHASE_FINAL;
}

static int proto_res_frame_status_valid(uint8_t status) {
  return status == PROTO_STATUS_OK || status == PROTO_STATUS_REJECTED ||
         status == PROTO_STATUS_FAILED;
}

static int proto_res_frame_error_code_valid(uint16_t error_code) {
  return error_code <= PROTOCOL_ERR_MSG_TOO_LARGE;
}

protocol_result_t encode_res_frame(const res_frame_t *frame, uint8_t *out) {
  uint16_t net_error_code = 0;

  if (frame == NULL || out == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }
  if (!proto_res_frame_phase_valid(frame->phase) ||
      !proto_res_frame_status_valid(frame->status) ||
      !proto_res_frame_error_code_valid(frame->error_code)) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  net_error_code = htons(frame->error_code);
  out[0] = frame->phase;
  out[1] = frame->status;
  memcpy(out + 2, &net_error_code, sizeof(net_error_code));
  return PROTOCOL_OK;
}

protocol_result_t decode_res_frame(res_frame_t *frame, const uint8_t *in) {
  uint16_t net_error_code = 0;

  if (frame == NULL || in == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  frame->phase = in[0];
  frame->status = in[1];
  memcpy(&net_error_code, in + 2, sizeof(net_error_code));
  frame->error_code = ntohs(net_error_code);

  if (!proto_res_frame_phase_valid(frame->phase) ||
      !proto_res_frame_status_valid(frame->status) ||
      !proto_res_frame_error_code_valid(frame->error_code)) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  return PROTOCOL_OK;
}

protocol_result_t send_res_frame(socket_t sock, const res_frame_t *frame) {
  uint8_t buf[HF_PROTOCOL_RES_FRAME_SIZE];
  ssize_t n = 0;
  protocol_result_t res = PROTOCOL_OK;

  if (is_socket_invalid(sock) || frame == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  res = encode_res_frame(frame, buf);
  if (res != PROTOCOL_OK) {
    return res;
  }

  n = send_all(sock, buf, sizeof(buf));
  if (n < (ssize_t)sizeof(buf)) {
    if (n >= 0) {
      return PROTOCOL_ERR_SHORT_WRITE;
    }
    return PROTOCOL_ERR_IO;
  }

  return PROTOCOL_OK;
}

protocol_result_t recv_res_frame(socket_t sock, res_frame_t *frame) {
  uint8_t buf[HF_PROTOCOL_RES_FRAME_SIZE];
  ssize_t n = 0;

  if (is_socket_invalid(sock) || frame == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  n = recv_all(sock, buf, sizeof(buf));
  if (n < (ssize_t)sizeof(buf)) {
    if (n < 0) {
      return PROTOCOL_ERR_IO;
    }
    return PROTOCOL_ERR_EOF;
  }

  return decode_res_frame(frame, buf);
}

int proto_get_file_name_len(const char *file_name, uint16_t *out_len) {
  size_t len = 0;

  if (file_name == NULL || out_len == NULL) return 1;

  len = strlen(file_name);
  if (len == 0 || len > HF_PROTOCOL_MAX_FILE_NAME_LEN)
    return 1;

  *out_len = (uint16_t)len;
  return 0;
}

size_t proto_file_transfer_prefix_size(uint16_t file_name_len) {
  return sizeof(uint16_t) + (size_t)file_name_len + sizeof(uint64_t);
}

protocol_result_t encode_file_prefix(const char *file_name,
                                     uint64_t content_size,
                                     uint8_t *out) {
  uint16_t file_name_len = 0;
  uint16_t net_len = 0;

  if (out == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }
  if (proto_get_file_name_len(file_name, &file_name_len) != 0) {
    return PROTOCOL_ERR_FILE_NAME_LEN;
  }

  net_len = htons(file_name_len);
  memcpy(out, &net_len, sizeof(net_len));
  out += sizeof(net_len);

  memcpy(out, file_name, (size_t)file_name_len);
  out += file_name_len;

  encode_u64_be(content_size, out);
  return PROTOCOL_OK;
}

protocol_result_t proto_send_file_transfer_prefix(socket_t sock,
                                                  const uint8_t *in,
                                                  size_t len) {
  ssize_t n = 0;

  if (is_socket_invalid(sock) || in == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  n = send_all(sock, in, len);
  if (n < (ssize_t)len) {
    if (n >= 0) {
      return PROTOCOL_ERR_SHORT_WRITE;
    }
    return PROTOCOL_ERR_IO;
  }

  return PROTOCOL_OK;
}

protocol_result_t proto_recv_file_transfer_prefix(socket_t sock,
                                                      char **file_name_out,
                                                      uint64_t *content_size_out) {
  uint16_t net_len = 0;
  uint16_t file_name_len = 0;
  char *file_name = NULL;
  uint8_t szbuf[8];

  if (file_name_out == NULL || content_size_out == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  *file_name_out = NULL;
  *content_size_out = 0;

  ssize_t n = recv_all(sock, &net_len, sizeof(net_len));
  if (n != (ssize_t)sizeof(net_len)) {
    return n < 0 ? PROTOCOL_ERR_IO : PROTOCOL_ERR_EOF;
  }

  file_name_len = ntohs(net_len);
  if (file_name_len == 0 || file_name_len > HF_PROTOCOL_MAX_FILE_NAME_LEN) {
    return PROTOCOL_ERR_FILE_NAME_LEN;
  }

  file_name = (char *)malloc((size_t)file_name_len + 1);
  if (file_name == NULL) {
    return PROTOCOL_ERR_ALLOC;
  }

  n = recv_all(sock, file_name, (size_t)file_name_len);
  if (n != (ssize_t)file_name_len) {
    free(file_name);
    return n < 0 ? PROTOCOL_ERR_IO : PROTOCOL_ERR_EOF;
  }
  file_name[file_name_len] = '\0';

  n = recv_all(sock, szbuf, sizeof(szbuf));
  if (n != (ssize_t)sizeof(szbuf)) {
    free(file_name);
    return n < 0 ? PROTOCOL_ERR_IO : PROTOCOL_ERR_EOF;
  }

  *file_name_out = file_name;
  *content_size_out = decode_u64_be(szbuf);
  return PROTOCOL_OK;
}

void init_header(protocol_header_t *header) {
  if (header == NULL) {
    return;
  }

  header->magic = HF_PROTOCOL_MAGIC; 
  header->version = HF_PROTOCOL_VERSION;
  header->msg_type = 0;
  header->flags = HF_MSG_FLAG_NONE;
  header->payload_size = 0;
}



protocol_result_t encode_header(const protocol_header_t *header, uint8_t *out) {
  if (header == NULL || out == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }
  
  uint16_t net_magic = htons(header->magic);
  uint8_t net_payload_size[8];
  encode_u64_be(header->payload_size, net_payload_size);
  
  uint8_t *base = out;
  memcpy(base, &net_magic, sizeof(net_magic)); base += sizeof(net_magic);
  *base++ = header->version;
  *base++ = header->msg_type;
  *base++ = header->flags;
  memcpy(base, net_payload_size, sizeof(net_payload_size));

  return PROTOCOL_OK;
}

protocol_result_t decode_header(protocol_header_t *header, const uint8_t *in) {

  if (in == NULL || header == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  uint16_t net_magic = 0;
  const uint8_t *base = in;

  memcpy(&net_magic, base, sizeof(net_magic));
  header->magic = ntohs(net_magic);
  if (header->magic != HF_PROTOCOL_MAGIC) {
    return PROTOCOL_ERR_HEADER_MAGIC;
  }
  base += sizeof(net_magic);

  header->version = *base++;
  if (header->version != HF_PROTOCOL_VERSION) {
    return PROTOCOL_ERR_HEADER_VERSION;
  }
  
  header->msg_type = *base++;
  if (header->msg_type != HF_MSG_TYPE_TEXT_MESSAGE &&
      header->msg_type != HF_MSG_TYPE_FILE_TRANSFER) {
    return PROTOCOL_ERR_HEADER_MSG_TYPE;
  }
  
  header->flags = *base++;
  if (header->flags != HF_MSG_FLAG_NONE) {
    return PROTOCOL_ERR_HEADER_MSG_FLAG;
  }

  header->payload_size = decode_u64_be(base);

  return PROTOCOL_OK;
}

protocol_result_t send_header(socket_t sock, const uint8_t *in) {
  if (is_socket_invalid(sock) || in == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }
  
  ssize_t n = send_all(sock, in, HF_PROTOCOL_HEADER_SIZE);
  if (n < HF_PROTOCOL_HEADER_SIZE) {
    if (n >= 0) return PROTOCOL_ERR_SHORT_WRITE;
    return PROTOCOL_ERR_IO;
  }
  
  return PROTOCOL_OK; 
}

protocol_result_t recv_header(socket_t sock, uint8_t *out) {
  if (is_socket_invalid(sock) || out == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }
  ssize_t n = recv_all(sock, out, HF_PROTOCOL_HEADER_SIZE);
  
  if (n < HF_PROTOCOL_HEADER_SIZE) {
    if (n < 0) return PROTOCOL_ERR_IO;
    return PROTOCOL_ERR_EOF; 
  }
  return PROTOCOL_OK;
}
