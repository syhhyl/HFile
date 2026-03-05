#ifndef HF_CLIENT_H
#define HF_CLIENT_H

#include <stdint.h>

int client(const char *path, const char *ip, uint16_t port, int perf);

#endif  // HF_CLIENT_H
