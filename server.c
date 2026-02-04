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



int server(char *path, uint16_t port) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);  
  if (listen_fd < 0) {
    perror("socket");
    return 1;
  }

  int opt = 1;
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt(SO_REUSEADDR)");
    return 1;
  }
  
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listen_fd, (struct sockaddr *)&addr,
      sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  
  listen(listen_fd, 1);

  int conn_flag = true;
  while (conn_flag) {
    printf("listening on %s port %u...\n", path, (unsigned)port);
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
    if (file_len == 0 || file_len > 255) {
      fprintf(stderr, "invalid file name length: %u\n", (unsigned)file_len);
      close(conn);
      continue;
    }

    char *file_name = (char *)malloc((size_t)file_len + 1);
    if (file_name == NULL) {
      perror("malloc(file_name)");
      close(conn);
      return 1;
    }
    ssize_t name_n = recv(conn, file_name, file_len, MSG_WAITALL);
    if (name_n != (ssize_t)file_len) {
      perror("recv(file_name)");
      free(file_name);
      close(conn);
      return 1;
    }
    file_name[file_len] = '\0';

    // Reject path traversal / separators; client is supposed to send only basename.
    if (strchr(file_name, '/') != NULL || strchr(file_name, '\\') != NULL ||
        strstr(file_name, "..") != NULL) {
      fprintf(stderr, "invalid file name: %s\n", file_name);
      free(file_name);
      close(conn);
      continue;
    }

    size_t file_path_len = strlen(path) + 1 + strlen(file_name) + 1;
    char *file_path = (char *)malloc(file_path_len);
    if (file_path == NULL) {
      perror("malloc(file_path)");
      free(file_name);
      close(conn);
      return 1;
    }
    int sn = snprintf(file_path, file_path_len, "%s/%s", path, file_name);
    free(file_name);
    if (sn < 0 || (size_t)sn >= file_path_len) {
      fprintf(stderr, "failed to build file path\n");
      free(file_path);
      close(conn);
      return 1;
    }


    //TODO unpack data from client: len | file_name | content
    int out = open(file_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) {
      perror("open");
      free(file_path);
      return 1;
    }


    while (1) {
      ssize_t n = read(conn, buf, sizeof(buf));
      if (n <= 0) break;
      write(out, buf, n);
    }
    printf("write file: %s\n", file_path);
    free(file_path);

    close(conn);
  }

  close(listen_fd);
  
  return 0;

  
}
