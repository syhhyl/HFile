#ifndef HF_NODE_H
#define HF_NODE_H

#include <stdint.h>

int node_recv(const char *dir, uint16_t port);
int node_send(const char *file_path, const char *ip, uint16_t port);

#endif  // HF_NODE_H
