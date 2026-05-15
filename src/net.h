#ifndef HF_NET_H
#define HF_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef int socket_t;

bool is_socket_invalid(socket_t sock);

ssize_t send_all(socket_t sock, const void *data, size_t len);
ssize_t recv_all(socket_t sock, void *buf, size_t len);

void sock_perror(const char *msg);
int socket_close(socket_t s);

#endif  /* HF_NET_H */
