#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include "client.h"

/*
get_file_name
find the last '/'
and copy to return
*/
int get_file_name(char **file_path, char **file_name) {
  if (*file_path == NULL) return 1;
  char *tmp = strrchr(*file_path, '/'); 
  if (tmp == NULL) *file_name = *file_path;
  else *file_name = tmp + 1;
  return *file_name ? 0 : 1;
}

int client(char *path, const char *ip, uint16_t port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    perror("socket");
    return 1;
  }
  
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    perror("inet_pton");
    goto CLOSE_SOCK;
  }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect");
    goto CLOSE_SOCK;
  }
  

  // char *file_path = path;
  char *file_name;
  
  if (get_file_name(&path, &file_name) != 0) {
    perror("get_file_name");
    goto CLOSE_SOCK;
  }

  size_t len = strlen(file_name);
  if (len > 255) {
    fprintf(stderr, "file name len > 255");
    goto CLOSE_SOCK;
  }
  uint16_t file_name_len = (uint16_t)len;

  
  int in = open(path, O_RDONLY);
  if (in == -1) {
    perror("open");
    goto CLOSE_SOCK;
  }

  char buf[4096];
  size_t offset = 0;
  uint16_t net_len = htons(file_name_len);
  memcpy(buf, &net_len, 2);
  offset += 2;
  memcpy(buf + offset, file_name, file_name_len);
  offset += file_name_len;

  
  bool first = true; 
  // TODO add write_all function
  for (;;) {
    size_t off = first ? offset : 0;
    ssize_t n = read(in, buf + off, sizeof(buf) - off);

    if (n <= 0 && !first) break;
  
    size_t to_send = off + (size_t)n;
    write(sock, buf, to_send);
    first = false;
  }
  

  close(in);

  return 0;

CLOSE_SOCK:
  close(sock);
  return 1; 
}

