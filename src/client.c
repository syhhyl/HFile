#include "helper.h"
#include "net.h"
#include "client.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include "fs.h"
#include <unistd.h>

int client(const char *path, const char *ip, uint16_t port) {
  int exit_code = 0;

#ifdef _WIN32
  SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == INVALID_SOCKET) {
#else
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
#endif
    sock_perror("socket");
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
    sock_perror("connect");
    exit_code = 1;
    goto CLOSE_SOCK;
  }
  

  const char *file_name;
  if (get_file_name(&path, &file_name) != 0) {
    fprintf(stderr, "invalid file path\n");
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


  int in;
  char *buf = NULL;
#ifdef _WIN32
  in = open(path, O_RDONLY | O_BINARY);
#else
  in = open(path, O_RDONLY);
#endif
  if (in == -1) {
    perror("open");
    exit_code = 1;
    goto CLOSE_SOCK;
  }

  if (sizeof(uint16_t) + file_name_len > CHUNK_SIZE) {
    fprintf(stderr, "file name too long for buffer\n");
    exit_code = 1;
    goto CLOSE_FILE;
  }

  buf = (char *)malloc(CHUNK_SIZE);
  if (buf == NULL) {
    perror("malloc(buf)");
    exit_code = 1;
    goto CLOSE_FILE;
  }

  uint16_t net_len = htons(file_name_len);
  
  memcpy(buf, &net_len, sizeof(net_len));
  memcpy(buf+sizeof(net_len), file_name, file_name_len);
  
  size_t pos = sizeof(net_len) + file_name_len;
  
  //time test
  double t_read = 0.0;
  double t_send = 0.0;
  size_t bytes_read = 0;
  size_t bytes_sent = 0;

  for (;;) {
    while (pos < CHUNK_SIZE) {
      double t0 = now_sec();
      ssize_t tmp = read(in, buf + pos, CHUNK_SIZE - pos);
      t_read += now_sec() - t0;
      
      if (tmp < 0) {
        perror("read");
        exit_code = 1;
        goto CLOSE_FILE;
      }
      if (tmp == 0) goto SEND_LAST;
      pos += (size_t)tmp;
      bytes_read += (size_t)tmp;
    }
    if (pos > 0) {
      double t0 = now_sec();
      ssize_t sent = send_all(sock, buf, pos);
      t_send += now_sec() - t0;
      if (sent != (ssize_t)pos) {
        sock_perror("send");
        exit_code = 1;
        goto CLOSE_FILE;
      }
      bytes_sent += (size_t)sent;
      pos = 0;
    }
  }

SEND_LAST:
  if (pos > 0) {
    double t0 = now_sec();
    ssize_t sent = send_all(sock, buf, pos);
    t_send += now_sec() - t0;
    if (sent != (ssize_t)pos) {
      sock_perror("send");
      exit_code = 1;
    } else bytes_sent += (size_t)sent; 
  }

  fprintf(stderr,
      "[timing] read: %.6fs (%.2f MiB/s), send: %.6fs (%.2f MiB/s)\n",
      t_read, t_read > 0 ? (bytes_read / (1024.0*1024.0)) / t_read : 0.0,
      t_send, t_send > 0 ? (bytes_sent / (1024.0*1024.0)) / t_send : 0.0);   

CLOSE_FILE:
  if (buf != NULL) {
    free(buf);
  }
  fd_close(in);

CLOSE_SOCK:
  socket_close(sock);



  return exit_code;
}
