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

typedef enum {
  NET_SEND_FILE_OK = 0,
  NET_SEND_FILE_UNSUPPORTED,
  NET_SEND_FILE_SOURCE_CHANGED,
  NET_SEND_FILE_IO,
  NET_SEND_FILE_INVALID_ARGUMENT
} net_send_file_result_t;

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


void encode_u64_be(uint64_t v, uint8_t out[8]);
uint64_t decode_u64_be(const uint8_t in[8]);
void encode_u32_be(uint32_t v, uint8_t out[4]);
uint32_t decode_u32_be(const uint8_t in[4]);

void sock_perror(const char *msg);

void socket_init(socket_t *s);
int socket_close(socket_t s);

net_send_file_result_t net_send_file_all(socket_t sock,
                                         int in_fd,
                                         uint64_t content_size);


#endif  // HF_NET_H
