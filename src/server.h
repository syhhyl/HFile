#ifndef HF_SERVER_H
#define HF_SERVER_H

#include <stdint.h>

typedef struct {
  const char *path;
  uint16_t port;
  int print_qr;
} server_opt_t;

int server(const server_opt_t *ser_opt);

#endif  // HF_SERVER_H
