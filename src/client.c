#include "helper.h"
#include <corecrt.h>
#include <windows.h>



int client(const char *path, const char *ip, uint16_t port) {
  int exit_code = 0;

#ifdef _WIN32
  SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == INVALID_SOCKET) {
#else
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
#endif
    perror("socket");
    exit_code = 1;
    return exit_code;
  }
  
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    perror("inet_pton");
    exit_code = 1;
    goto CLOSE_SOCK;
  }

#ifdef _WIN32
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
#else
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#endif
    perror("connect");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  

  const char *file_name;
  if (get_file_name(&path, &file_name) != 0) {
    perror("get_file_name");
    exit_code = 1;
    goto CLOSE_SOCK;
  }

  size_t len = strlen(file_name);
  if (len > 255) {
    fprintf(stderr, "file name len > 255");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  uint16_t file_name_len = (uint16_t)len;


  int in = open(path, O_RDONLY);
  if (in == -1) {
    perror("open");
    exit_code = 1;
    goto CLOSE_SOCK;
  }

  char buf[CHUNK_SIZE];
  uint16_t net_len = htons(file_name_len);
  
  memcpy(buf, &net_len, sizeof(net_len));
  memcpy(buf+sizeof(net_len), file_name, file_name_len);
  
  size_t pos = sizeof(net_len) + file_name_len;

  while (1) {
    while (pos < CHUNK_SIZE) {
      ssize_t tmp = read(in, buf+pos, CHUNK_SIZE-pos);
      if (tmp < 0) {
        perror("read");
        exit_code = 1;
        goto CLOSE_FILE;
      }
      if (tmp == 0) goto SEND_LAST;
      pos += (size_t)tmp;
    }
    if (pos > 0) {
      ssize_t sent = send_all(sock, buf, pos);
      if (sent != (ssize_t)pos) {
        fprintf(stderr, "send failed\n");
        exit_code = 1;
        break;
      }
      pos = 0;
    }
  }

SEND_LAST:
  if (pos > 0) {
    ssize_t sent = send_all(sock, buf, pos);
    if (sent != (ssize_t)pos) {
      fprintf(stderr, "send failed\n");
      exit_code = 1;
    }
  }

  
CLOSE_FILE:
  close(in);

CLOSE_SOCK:
  close(sock);

  return exit_code;
}