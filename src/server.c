#include <sys/socket.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "errno.h"
#include "helper.h"



int server(const char *path, uint16_t port) {
  if (path == NULL)
    goto PATH_ERROR;

  int sock = socket(AF_INET, SOCK_STREAM, 0);  
  if (sock < 0) {
    perror("socket");
    return 1;
  }

  int opt = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt(SO_REUSEADDR)");
    goto CLOSE_SOCK;
  }
  
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    goto CLOSE_SOCK;
  }
  
  if (listen(sock, 1) == -1) {
    perror("listen");
    goto CLOSE_SOCK;
  }
  

  int conn_flag = true;
  printf("listening on %s port %u...\n", path, (unsigned)port);

  while (conn_flag) {
    int conn = accept(sock, NULL, NULL);
    if (conn < 0) {
      if (errno == EINTR)
        continue;
      perror("accept");
      continue;
    }
    printf("client connected\n");


    char buf[CHUNK_SIZE];
    
    //get file name len
    uint16_t file_len;
    if (read_all(conn, &file_len, 2) != 2) {
      perror("read(file_len)");
      goto CLOSE_CONN;
    }
    
    file_len = ntohs(file_len);
    
    
    //get file_name
    if (file_len == 0 || file_len > 255) {
      fprintf(stderr, "invalid file name length: %u\n", (unsigned)file_len);
      goto CLOSE_CONN;
    }

    char *file_name = (char *)malloc((size_t)file_len + 1);
    if (file_name == NULL) {
      perror("malloc(file_name)");
      goto CLOSE_CONN;
    }
    
    //get file name
    // ssize_t name_n = recv(conn, file_name, file_len, MSG_WAITALL);
    // ssize_t name_n = read_all(conn, file_name, file_len);
    if (read_all(conn, file_name, (size_t)file_len) != (ssize_t)file_len) {
      perror("read(file_name)");
      free(file_name);
      goto CLOSE_CONN;
    }
    file_name[file_len] = '\0';

    if (strchr(file_name, '/') != NULL || strchr(file_name, '\\') != NULL ||
        strstr(file_name, "..") != NULL) {
      fprintf(stderr, "invalid file name: %s\n", file_name);
      free(file_name);
      goto CLOSE_CONN;
    }

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
      perror("open(output_dir)");
      free(file_name);
      close(conn);
      goto CLOSE_SOCK;
    }

    int out = openat(dirfd, file_name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    close(dirfd);
    if (out < 0) {
      perror("open");
      free(file_name);
      goto CLOSE_CONN;
    }


    //start send
    clock_t start = clock(); 
    for (;;) {
      ssize_t n = read(conn, buf, sizeof(buf));
      if (n > 0) {
        if (write_all(out, buf, (size_t)n) < 0) {
          perror("write");
          break;
        }
      } else if (n == 0) break;
      else {
        if (errno == EINTR) continue;
        else {
          perror("read");
          break;
        }
      }
    }
    clock_t end = clock();
    
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    size_t path_len = strlen(path);
    const char *sep = (path_len > 0 && path[path_len - 1] == '/') ? "" : "/";
    printf("write file: %s%s%s\t%lfs\n", path, sep, file_name, elapsed);

    free(file_name);
    close(out);

CLOSE_CONN:
    close(conn);
  }
  
PATH_ERROR:
  return 0;

CLOSE_SOCK:
  close(sock);
  return 1;
}
