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

#define CHUNK_SIZE 1024 * 1024

bool is_socket_invalid(socket_t sock);

typedef enum {
  NET_SEND_FILE_OK = 0,
  NET_SEND_FILE_UNSUPPORTED,
  NET_SEND_FILE_SOURCE_CHANGED,
  NET_SEND_FILE_IO,
  NET_SEND_FILE_INVALID_ARGUMENT
} net_send_file_result_t;

typedef enum {
  NET_RECV_FILE_OK = 0,
  NET_RECV_FILE_UNSUPPORTED,
  NET_RECV_FILE_EOF,
  NET_RECV_FILE_IO,
  NET_RECV_FILE_INVALID_ARGUMENT
} net_recv_file_result_t;

ssize_t send_all(
  socket_t sock,
  const void *data, size_t len);

ssize_t recv_all(
  socket_t sock,
  void *buf, size_t len);


void sock_perror(const char *msg);

void socket_init(socket_t *s);
int socket_close(socket_t s);

int net_wait_readable(socket_t sock, uint32_t timeout_ms, int *ready_out);

net_send_file_result_t net_send_file_best_effort(socket_t sock,
                                                  int in_fd,
                                                  uint64_t content_size);
net_recv_file_result_t net_recv_file_best_effort(socket_t sock,
                                                  int out_fd,
                                                  uint64_t content_size);


#endif  // HF_NET_H
