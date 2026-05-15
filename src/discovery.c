#include "discovery.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define DISCOVERY_QUERY_BYTES 3u
#define DISCOVERY_RESPONSE_BYTES 5u

int discovery_open(socket_t *sock_out, uint16_t tcp_port) {
  if (sock_out == NULL) return 1;

  socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (is_socket_invalid(sock)) {
    perror("discovery socket");
    return 1;
  }

  int opt = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("discovery setsockopt(SO_REUSEADDR)");
    goto fail;
  }
  if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) < 0) {
    perror("discovery setsockopt(SO_BROADCAST)");
    goto fail;
  }

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(tcp_port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("discovery bind");
    goto fail;
  }

  *sock_out = sock;
  return 0;

fail:
  socket_close(sock);
  return 1;
}

void discovery_close(socket_t sock) {
  socket_close(sock);
}

int discovery_handle_query(socket_t sock, uint16_t tcp_port) {
  uint8_t query[DISCOVERY_QUERY_BYTES];
  struct sockaddr_in from;
  socklen_t from_len = sizeof(from);

  ssize_t n = recvfrom(sock, query, sizeof(query), 0,
                       (struct sockaddr *)&from, &from_len);
  if (n < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    perror("discovery recvfrom");
    return 1;
  }

  if ((size_t)n != sizeof(query)) return 0;
  if ((((uint16_t)query[0] << 8) | query[1]) != HF_DISCOVERY_MAGIC ||
      query[2] != HF_DISCOVERY_VERSION) {
    return 0;
  }

  uint8_t resp[DISCOVERY_RESPONSE_BYTES];
  resp[0] = (uint8_t)(HF_DISCOVERY_MAGIC >> 8);
  resp[1] = (uint8_t)(HF_DISCOVERY_MAGIC);
  resp[2] = HF_DISCOVERY_VERSION;
  resp[3] = (uint8_t)(tcp_port >> 8);
  resp[4] = (uint8_t)(tcp_port);

  ssize_t ns = sendto(sock, resp, sizeof(resp), 0,
                      (struct sockaddr *)&from, from_len);
  if (ns < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    perror("discovery sendto");
    return 1;
  }

  return 0;
}

int discovery_find_node(uint16_t port, char *ip_out, size_t ip_out_len,
                        uint16_t *port_out) {
  if (ip_out == NULL || ip_out_len == 0 || port_out == NULL) return 1;
  int ret = 1;

  socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (is_socket_invalid(sock)) {
    perror("discovery socket");
    return 1;
  }

  int opt = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) < 0) {
    perror("discovery setsockopt(SO_BROADCAST)");
    goto done;
  }

  uint8_t req[DISCOVERY_QUERY_BYTES];
  req[0] = (uint8_t)(HF_DISCOVERY_MAGIC >> 8);
  req[1] = (uint8_t)(HF_DISCOVERY_MAGIC);
  req[2] = HF_DISCOVERY_VERSION;

  struct sockaddr_in broadcast_addr = {0};
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_port = htons(port);
  broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

  if (sendto(sock, req, sizeof(req), 0,
             (struct sockaddr *)&broadcast_addr,
              sizeof(broadcast_addr)) < 0) {
    perror("discovery broadcast sendto");
    goto done;
  }

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(sock, &readfds);
  struct timeval tv = {
    .tv_sec = (long)(HF_DISCOVERY_TIMEOUT_MS / 1000u),
    .tv_usec = (long)((HF_DISCOVERY_TIMEOUT_MS % 1000u) * 1000u),
  };

  int rc = select(sock + 1, &readfds, NULL, NULL, &tv);
  if (rc < 0) {
    if (errno != EINTR) perror("discovery select");
    goto done;
  }

  if (rc == 0) goto done;

  uint8_t resp[DISCOVERY_RESPONSE_BYTES];
  struct sockaddr_in from;
  socklen_t from_len = sizeof(from);

  ssize_t n = recvfrom(sock, resp, sizeof(resp), 0,
                        (struct sockaddr *)&from, &from_len);
  if (n < 0) {
    perror("discovery recvfrom");
    goto done;
  }

  if ((size_t)n != sizeof(resp)) goto done;
  if ((((uint16_t)resp[0] << 8) | resp[1]) != HF_DISCOVERY_MAGIC ||
      resp[2] != HF_DISCOVERY_VERSION) {
    goto done;
  }

  if (inet_ntop(AF_INET, &from.sin_addr, ip_out, (socklen_t)ip_out_len) == NULL) {
    perror("discovery inet_ntop");
    goto done;
  }

  *port_out = ((uint16_t)resp[3] << 8) | resp[4];
  ret = 0;

done:
  socket_close(sock);
  return ret;
}
