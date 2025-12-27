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

  //TODO send file_name_len and file_name to buf
  // file_name_len | file_name | file_content
  char *file_path = argv[1];
  
  // char *begin = file_path;
  // size_t offset = strlen(file_path) - 1;
  // char *end = file_path + offset;
  // size_t file_name_len = 0;
  // while ((*end) != '/') {
  //   file_name_len++; 
  //   --end; 
  // }
  // end++;
  // char *file_name = (char *)malloc(sizeof(char) * (file_name_len + 1));
  // for (size_t i = 0; i < file_name_len; ++i) {
  //   file_name[i] = (*end++);
  // }
  // file_name[file_name_len+1] = '\0';
  // printf("file_name: %s, file_name_len: %lu", file_name, file_name_len);
  
  char *file_name;
  
  get_file_name(file_path, &file_name);
  
  printf("file_name: %s\n", file_name);
  
  
   
  
  int in = open(file_path, O_RDONLY);
  char buf[4096];
  while (1) {
    ssize_t n = read(in, buf, sizeof(buf));
    if (n <= 0) break;
    write(sock, buf, n);
  }
}