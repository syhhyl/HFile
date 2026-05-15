#include "discovery.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

int discovery_open(socket_t *sock_out, uint16_t tcp_port) {
  if (sock_out == NULL) return 1;

  socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (is_socket_invalid(sock)) {
    sock_perror("discovery socket");
    return 1;
  }

  {
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
      sock_perror("discovery setsockopt(SO_REUSEADDR)");
      socket_close(sock);
      return 1;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) < 0) {
      sock_perror("discovery setsockopt(SO_BROADCAST)");
      socket_close(sock);
      return 1;
    }
  }

  (void)tcp_port;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(tcp_port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    sock_perror("discovery bind");
    socket_close(sock);
    return 1;
  }

  *sock_out = sock;
  return 0;
}

void discovery_close(socket_t sock) {
  socket_close(sock);
}

int discovery_handle_query(socket_t sock, uint16_t tcp_port) {
  uint8_t buf[3];
  struct sockaddr_in from;
  socklen_t from_len = sizeof(from);

  ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                       (struct sockaddr *)&from, &from_len);
  if (n < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    sock_perror("discovery recvfrom");
    return 1;
  }

  if ((size_t)n < sizeof(buf)) return 0;

  uint16_t magic = ((uint16_t)buf[0] << 8) | buf[1];
  uint8_t version = buf[2];

  if (magic != HF_DISCOVERY_MAGIC || version != HF_DISCOVERY_VERSION) {
    return 0;
  }

  uint8_t resp[5];
  resp[0] = (uint8_t)(HF_DISCOVERY_MAGIC >> 8);
  resp[1] = (uint8_t)(HF_DISCOVERY_MAGIC);
  resp[2] = HF_DISCOVERY_VERSION;
  resp[3] = (uint8_t)(tcp_port >> 8);
  resp[4] = (uint8_t)(tcp_port);

  ssize_t ns = sendto(sock, resp, sizeof(resp), 0,
                      (struct sockaddr *)&from, from_len);
  if (ns < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    sock_perror("discovery sendto");
    return 1;
  }

  return 0;
}

int discovery_find_node(uint16_t port, char *ip_out, size_t ip_out_len,
                        uint16_t *port_out) {
  if (ip_out == NULL || ip_out_len == 0 || port_out == NULL) return 1;

  socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (is_socket_invalid(sock)) {
    sock_perror("discovery socket");
    return 1;
  }

  {
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
  }

  uint8_t req[3];
  req[0] = (uint8_t)(HF_DISCOVERY_MAGIC >> 8);
  req[1] = (uint8_t)(HF_DISCOVERY_MAGIC);
  req[2] = HF_DISCOVERY_VERSION;

  struct sockaddr_in broadcast_addr;
  memset(&broadcast_addr, 0, sizeof(broadcast_addr));
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_port = htons(port);
  broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

  if (sendto(sock, req, sizeof(req), 0,
             (struct sockaddr *)&broadcast_addr,
             sizeof(broadcast_addr)) < 0) {
    sock_perror("discovery broadcast sendto");
    socket_close(sock);
    return 1;
  }

  struct timeval tv;
  tv.tv_sec = (long)(HF_DISCOVERY_TIMEOUT_MS / 1000u);
  tv.tv_usec = (long)((HF_DISCOVERY_TIMEOUT_MS % 1000u) * 1000u);

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(sock, &readfds);

  int rc = select(sock + 1, &readfds, NULL, NULL, &tv);
  if (rc < 0) {
    if (errno == EINTR) {
      socket_close(sock);
      return 1;
    }
    sock_perror("discovery select");
    socket_close(sock);
    return 1;
  }

  if (rc == 0) {
    socket_close(sock);
    return 1;
  }

  uint8_t resp[5];
  struct sockaddr_in from;
  socklen_t from_len = sizeof(from);

  ssize_t n = recvfrom(sock, resp, sizeof(resp), 0,
                       (struct sockaddr *)&from, &from_len);
  if (n < 0) {
    sock_perror("discovery recvfrom");
    socket_close(sock);
    return 1;
  }

  socket_close(sock);

  if ((size_t)n < sizeof(resp)) return 1;

  uint16_t magic = ((uint16_t)resp[0] << 8) | resp[1];
  uint8_t version = resp[2];

  if (magic != HF_DISCOVERY_MAGIC || version != HF_DISCOVERY_VERSION) {
    return 1;
  }

  if (inet_ntop(AF_INET, &from.sin_addr, ip_out, (socklen_t)ip_out_len) == NULL) {
    perror("discovery inet_ntop");
    return 1;
  }

  *port_out = ((uint16_t)resp[3] << 8) | resp[4];
  return 0;
}
