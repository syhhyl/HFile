/**
 * @file server.c
 * @brief Server implementation for HFile
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "helper.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <errno.h>
#ifdef _MSC_VER
#define EINTR WSAEINTR
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

/**
 * @brief Start HFile server
 * @param path  Output directory for received files
 * @param port  Listening port
 * @return 0 on success, 1 on error
 */
int server(const char *path, uint16_t port) {
  if (path == NULL) {
    goto PATH_ERROR;
  }

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return 1;
  }

#ifdef _WIN32
  char opt = 1;
#else
  int opt = 1;
#endif
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
      if (errno == EINTR) {
        continue;
      }
      perror("accept");
      continue;
    }
    printf("client connected\n");

    char buf[CHUNK_SIZE];

    /* Get file name length (2 bytes, network byte order) */
    uint16_t file_len;
    if (read_all(conn, &file_len, 2) != 2) {
      perror("read(file_len)");
      goto CLOSE_CONN;
    }

    file_len = ntohs(file_len);

    if (file_len == 0 || file_len > 255) {
      fprintf(stderr, "invalid file name length: %u\n", (unsigned)file_len);
      goto CLOSE_CONN;
    }

    /* Read file name */
    char *file_name = (char *)malloc((size_t)file_len + 1);
    if (file_name == NULL) {
      perror("malloc(file_name)");
      goto CLOSE_CONN;
    }

    if (read_all(conn, file_name, (size_t)file_len) != (ssize_t)file_len) {
      perror("read(file_name)");
      free(file_name);
      goto CLOSE_CONN;
    }
    file_name[file_len] = '\0';

    /* Security check: reject path traversal attempts */
    /* Reject: ../foo or ..\foo (parent directory traversal) */
    if (file_name[0] == '.' && file_name[1] == '.') {
      if (file_name[2] == '/' || file_name[2] == '\\') {
        fprintf(stderr, "invalid file name: %s\n", file_name);
        free(file_name);
        goto CLOSE_CONN;
      }
    }

    /* Reject Unix path separators */
    if (strchr(file_name, '/') != NULL) {
      fprintf(stderr, "invalid file name: %s\n", file_name);
      free(file_name);
      goto CLOSE_CONN;
    }

    /* Open output file */
#ifndef _WIN32
    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
      perror("open(output_dir)");
      free(file_name);
      close_socket(conn);
      goto CLOSE_SOCK;
    }

    int out = openat(dirfd, file_name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    close(dirfd);
#else
    /* Windows: build full path manually */
    size_t full_path_len = strlen(path) + 1 + file_len + 1;
    char *full_path = (char *)malloc(full_path_len);
    if (full_path == NULL) {
      perror("malloc");
      free(file_name);
      goto CLOSE_CONN;
    }
    if (path[strlen(path) - 1] == '\\' || path[strlen(path) - 1] == '/') {
      snprintf(full_path, full_path_len, "%s%s", path, file_name);
    } else {
      snprintf(full_path, full_path_len, "%s\\%s", path, file_name);
    }
    int out = _open(full_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    free(full_path);
#endif

    if (out < 0) {
      perror("open");
      free(file_name);
      goto CLOSE_CONN;
    }

    /* Receive file data */
    clock_t start = clock();
    for (;;) {
#ifdef _WIN32
      ssize_t n = recv(conn, buf, (int)sizeof(buf), 0);
#else
      ssize_t n = read(conn, buf, sizeof(buf));
#endif
      if (n > 0) {
        if (write_all(out, buf, (size_t)n) < 0) {
          perror("write");
          break;
        }
      } else if (n == 0) {
        break;
      } else {
        if (errno == EINTR) {
          continue;
        } else {
          perror("read");
          break;
        }
      }
    }

    clock_t end = clock();

    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    size_t path_len = strlen(path);
    // const char *sep = (path_len > 0 && (path[path_len - 1] == '/' || path[path_len - 1] == '\\')) ? "" : "/";
    // printf("write file: %s%s%s\t%lfs\n", path, sep, file_name, elapsed);

    free(file_name);
    close(out);

CLOSE_CONN:
    close_socket(conn);
  }

PATH_ERROR:
  return 0;

CLOSE_SOCK:
  close_socket(sock);
  return 1;
}
