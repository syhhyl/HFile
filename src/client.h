#ifndef HF_CLIENT_H
#define HF_CLIENT_H

#include <stdint.h>

typedef struct {
  const char *path;
  const char *ip;
  uint16_t port;
} client_opt_t;

int client(const client_opt_t *cli_opt);

#endif  // HF_CLIENT_H
