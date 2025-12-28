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
#include "utils.h"

typedef struct {
  char *name[30];
  uint8_t len;
} name_block;


int main(int argc, char **argv) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9000);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  
  if (connect(sock, (struct sockaddr *)&addr,
      sizeof(addr)) < 0) {
    perror("connect");
    return 1;
  }
  
  //TODO give a file path
  if (argc == 1) {
    printf("no path\n");
    return 1;
  }


  char *file_path = argv[1];
  char *file_name;
  
  get_file_name(file_path, &file_name);
  uint16_t file_name_len = (uint16_t)strlen(file_name); 

  printf("name: %s len: %d\n", file_name, file_name_len);
  
  
  int in = open(file_path, O_RDONLY);
  char buf[4096];
  size_t offset = 0;
  uint16_t len_be = htons((uint16_t)file_name_len);
  memcpy(buf, &len_be, 2);
  offset += 2;
  memcpy(buf + offset, file_name, file_name_len);
  offset += file_name_len;

  
// #ifdef SEND
  bool first = true; 
  for (;;) {
    size_t off = first ? offset : 0;
    ssize_t n = read(in, buf + off, sizeof(buf) - off);
    if (n <= 0) break;
    
    size_t to_send = off + (size_t)n;
    write(sock, buf, to_send);
    first = 0;
  }
// #endif
}

