#include <sys/socket.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "server.h"




int server(char *path) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);  
  if (listen_fd < 0) {
    perror("socket");
    return 1;
  }
  
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9000);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listen_fd, (struct sockaddr *)&addr,
      sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  
  listen(listen_fd, 1);

  int conn_flag = true;
  while (conn_flag) {
    printf("listening on %s port 9000...\n", path);
    int conn = accept(listen_fd, NULL, NULL);
    printf("client connected\n");


    char buf[4096]; // 4096B = 4KB
    uint16_t file_len;
    ssize_t n = recv(conn, &file_len, 2, MSG_WAITALL);
    
    printf("n:%ld, sizeof(file_len):%zu\n", n, sizeof(file_len));
    if (n != 2) {
      perror("read len");
      return 1;
    }
    
    
    file_len = ntohs(file_len);
    
    
    //get file_name
    char *file_name = (char *)malloc(file_len + 1);
    read(conn, file_name, file_len);
    file_name[file_len] = '\0';
    char file_path[100];


    
    snprintf(file_path, sizeof(file_path), "%s/%s", path, file_name);


    //TODO unpack data from client: len | file_name | content
    int out = open(file_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) {
      perror("open");
      return 1;
    }


    while (1) {
      ssize_t n = read(conn, buf, sizeof(buf));
      if (n <= 0) break;
      write(out, buf, n);
    }
    printf("write file: %s\n", file_path);

    close(conn);
  }

  close(listen_fd);
  
  return 0;

  
}