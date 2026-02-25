#ifndef HF_NET_H
#define HF_NET_H

#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <io.h>

  #ifndef O_BINARY
    #ifdef _O_BINARY
      #define O_BINARY _O_BINARY
    #else
      #define O_BINARY 0
    #endif
  #endif

  // ssize_t is POSIX; provide it on Windows toolchains that lack it.
  #if defined(__MINGW32__) || defined(__MINGW64__)
    #include <sys/types.h>
  #else
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
  #endif

  #define read _read
  #define write _write
  #define open _open

  // Keep SOCKET and file descriptors separate on Windows.
  static inline int socket_close(SOCKET s) { return closesocket(s); }
  static inline int fd_close(int fd) { return _close(fd); }
   
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <sys/types.h>

  static inline int socket_close(int s) { return close(s); }
  static inline int fd_close(int fd) { return close(fd); }
#endif


#define CHUNK_SIZE 1024 * 1024

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

void sock_perror(const char *msg);

#endif  // HF_NET_H
