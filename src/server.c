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
    if (conn < 0) {
      perror("accept");
      continue;
    }
    printf("client connected\n");


    char buf[4096]; // 4096B = 4KB
    uint16_t file_len;
    ssize_t n = recv(conn, &file_len, 2, MSG_WAITALL);
    
    printf("n:%ld, sizeof(file_len):%zu\n", n, sizeof(file_len));
    if (n == 0) {
      // Client connected and closed immediately.
      close(conn);
      continue;
    }
    if (n != 2) {
      perror("recv(file_len)");
      close(conn);
      continue;
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
      continue;
    }
    ssize_t name_n = recv(conn, file_name, file_len, MSG_WAITALL);
    if (name_n != (ssize_t)file_len) {
      perror("recv(file_name)");
      free(file_name);
      close(conn);
      continue;
    }
    file_name[file_len] = '\0';

    if (strchr(file_name, '/') != NULL || strchr(file_name, '\\') != NULL ||
        strstr(file_name, "..") != NULL) {
      fprintf(stderr, "invalid file name: %s\n", file_name);
      free(file_name);
      close(conn);
      continue;
    }

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
      perror("open(output_dir)");
      free(file_name);
      close(conn);
      continue;
    }

    int out = openat(dirfd, file_name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    close(dirfd);
    if (out < 0) {
      perror("open");
      free(file_name);
      close(conn);
      continue;
    }


    while (1) {
      ssize_t n = read(conn, buf, sizeof(buf));
      if (n <= 0) break;
      write(out, buf, n);
    }

    size_t path_len = strlen(path);
    const char *sep = (path_len > 0 && path[path_len - 1] == '/') ? "" : "/";
    printf("write file: %s%s%s\n", path, sep, file_name);
    free(file_name);

    close(out);

    close(conn);
  }

  close(listen_fd);
  
  return 0;

  
}
