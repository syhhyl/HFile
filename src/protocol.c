#include "protocol.h"
#include "net.h"

#include <stdlib.h>
#include <string.h>

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

size_t proto_compressed_block_size(uint32_t stored_size) {
  return HF_COMPRESS_BLOCK_HEADER_SIZE + (size_t)stored_size;
}

protocol_result_t proto_send_file_transfer_prefix(socket_t sock,
                                                     const char *file_name,
                                                     uint64_t content_size) {
  uint16_t file_name_len = 0;
  uint16_t net_len = 0;
  uint8_t szbuf[8];

  if (proto_get_file_name_len(file_name, &file_name_len) != 0) {
    return PROTOCOL_ERR_FILE_NAME_LEN;
  }

  net_len = htons(file_name_len);
  if (send_all(sock, &net_len, sizeof(net_len)) != (ssize_t)sizeof(net_len)) {
    return PROTOCOL_ERR_IO;
  }
  if (send_all(sock, file_name, (size_t)file_name_len) !=
      (ssize_t)file_name_len) {
    return PROTOCOL_ERR_IO;
  }

  encode_u64_be(content_size, szbuf);
  if (send_all(sock, szbuf, sizeof(szbuf)) != (ssize_t)sizeof(szbuf)) {
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

protocol_result_t proto_encode_compressed_block_header(
  uint8_t *out,
  uint8_t block_type,
  uint32_t raw_size,
  uint32_t stored_size) {
  uint8_t *base = out;

  if (out == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  *base++ = block_type;
  encode_u32_be(raw_size, base);
  base += sizeof(uint32_t);
  encode_u32_be(stored_size, base);
  return PROTOCOL_OK;
}

protocol_result_t proto_decode_compressed_block_header(
  const uint8_t *in,
  uint8_t *block_type_out,
  uint32_t *raw_size_out,
  uint32_t *stored_size_out) {
  const uint8_t *base = in;

  if (in == NULL || block_type_out == NULL || raw_size_out == NULL ||
      stored_size_out == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }

  *block_type_out = *base++;
  *raw_size_out = decode_u32_be(base);
  base += sizeof(uint32_t);
  *stored_size_out = decode_u32_be(base);
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
  base += sizeof(net_magic);

  header->version = *base++;
  header->msg_type = *base++;
  header->flags = *base++;
  header->payload_size = decode_u64_be(base);

  return PROTOCOL_OK;
}

protocol_result_t send_header(socket_t sock, const uint8_t *in) {
  if (in == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }
  if (send_all(sock, in, HF_PROTOCOL_HEADER_SIZE) != (ssize_t)HF_PROTOCOL_HEADER_SIZE) {
    return PROTOCOL_ERR_IO;
  }
  return PROTOCOL_OK; 
}

protocol_result_t recv_header(socket_t sock, uint8_t *out) {
  if (out == NULL) {
    return PROTOCOL_ERR_INVALID_ARGUMENT;
  }
  ssize_t n = recv_all(sock, out, HF_PROTOCOL_HEADER_SIZE);
  
  if (n < HF_PROTOCOL_HEADER_SIZE) {
    if (n < 0) return PROTOCOL_ERR_IO;
    return PROTOCOL_ERR_EOF; 
  }
  return PROTOCOL_OK;
}
