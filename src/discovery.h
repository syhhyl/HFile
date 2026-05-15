#ifndef HF_DISCOVERY_H
#define HF_DISCOVERY_H

#include <stdint.h>
#include "net.h"

#define HF_DISCOVERY_MAGIC   0x042A
#define HF_DISCOVERY_VERSION 0x01

#define HF_DISCOVERY_TIMEOUT_MS 3000u

int discovery_open(socket_t *sock_out, uint16_t tcp_port);
void discovery_close(socket_t sock);
int discovery_handle_query(socket_t sock, uint16_t tcp_port);

int discovery_find_node(uint16_t port, char *ip_out, size_t ip_out_len,
                        uint16_t *port_out);

#endif  /* HF_DISCOVERY_H */
