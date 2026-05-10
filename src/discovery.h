#ifndef HF_DISCOVERY_H
#define HF_DISCOVERY_H

#include <stdint.h>
#include "net.h"

#define HF_DISCOVERY_MAGIC   0x042A
#define HF_DISCOVERY_VERSION 0x01

#define HF_DISCOVERY_TIMEOUT_MS 3000u

int discovery_server_open(socket_t *sock_out, uint16_t tcp_port);
void discovery_server_close(socket_t sock);
int discovery_server_handle(socket_t sock, uint16_t tcp_port);

int discovery_client_find(uint16_t port, char *ip_out, size_t ip_out_len,
                          uint16_t *port_out);

#endif  /* HF_DISCOVERY_H */
