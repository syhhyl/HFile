#ifndef HF_NET_H
#define HF_NET_H


#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET socket_t;

  // ssize_t is POSIX; provide it on Windows toolchains that lack it.
  #if defined(__MINGW32__) || defined(__MINGW64__)
    #include <sys/types.h>
  #else
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
  #endif

   
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <sys/types.h>
  typedef int socket_t;


#endif



int net_init();
void net_cleanup();

ssize_t send_all(
#ifdef _WIN32
  SOCKET sock,
#else
  int sock,
#endif
  const void *data, size_t len);

ssize_t recv_all(
#ifdef _WIN32
  SOCKET sock,
#else
  int sock,
#endif
  void *buf, size_t len);

ssize_t write_all(int fd, const void *buf, size_t len);

void encode_u64_be(uint64_t v, uint8_t out[8]);
uint64_t decode_u64_be(const uint8_t in[8]);

void sock_perror(const char *msg);

int socket_close(socket_t s);

//uint64_t encode/decode


#endif  // HF_NET_H
