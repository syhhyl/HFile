#include "protocol.h"

#include <stdlib.h>
#include <string.h>

int protocol_get_file_name_len(const char *file_name, uint16_t *out_len) {
  size_t len = 0;

  if (file_name == NULL || out_len == NULL) {
    return 1;
  }

  len = strlen(file_name);
  if (len == 0 || len > HF_PROTOCOL_MAX_FILE_NAME_LEN) {
    return 1;
  }

  *out_len = (uint16_t)len;
  return 0;
}

size_t protocol_header_size(uint16_t file_name_len) {
  return sizeof(uint16_t) + (size_t)file_name_len + sizeof(uint64_t);
}

protocol_result_t protocol_send_header(socket_t sock, const char *file_name,
                                       uint64_t content_size) {
  uint16_t file_name_len = 0;
  uint16_t net_len = 0;
  uint8_t szbuf[8];

  if (protocol_get_file_name_len(file_name, &file_name_len) != 0) {
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

protocol_result_t protocol_recv_header(socket_t sock, char **file_name_out,
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
