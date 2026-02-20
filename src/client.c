/**
 * @file client.c
 * @brief Client implementation for HFile
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
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
 * @brief Extract filename from full path
 * @param file_path  Full path (input)
 * @param file_name  Extracted filename (output)
 * @return 0 on success, 1 on error
 *
 * Handles both Unix (/) and Windows (\) path separators.
 */
int get_file_name(const char **file_path, const char **file_name) {
  if (*file_path == NULL) {
    return 1;
  }

  /* Try Unix separator first */
  char *tmp = strrchr(*file_path, '/');

#ifdef _WIN32
  /* Try Windows separator */
  char *tmp_win = strrchr(*file_path, '\\');
  if (tmp == NULL || (tmp_win != NULL && tmp_win > tmp)) {
    tmp = tmp_win;
  }
#endif

  if (tmp == NULL) {
    *file_name = *file_path;
  } else {
    *file_name = tmp + 1;
  }

  return *file_name ? 0 : 1;
}

/**
 * @brief Connect to HFile server and send file
 * @param path  File path to send
 * @param ip    Server IP address
 * @param port  Server port
 * @return 0 on success, 1 on error
 */
int client(const char *path, const char *ip, uint16_t port) {
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

  /* Extract filename from path */
  const char *file_name;
  if (get_file_name(&path, &file_name) != 0) {
    fprintf(stderr, "get_file_name failed\n");
    goto CLOSE_SOCK;
  }

  size_t len = strlen(file_name);
  if (len > 255) {
    fprintf(stderr, "file name too long (max 255 bytes)\n");
    goto CLOSE_SOCK;
  }
  uint16_t file_name_len = (uint16_t)len;

  /* Open input file */
  int in = open(path, O_RDONLY);
  if (in == -1) {
    perror("open");
    goto CLOSE_SOCK;
  }

  char buf[CHUNK_SIZE];

  /* Send protocol: 2-byte length (network order) + filename + file data */
  clock_t start = clock();

  uint16_t net_len = htons(file_name_len);
  write_all(sock, &net_len, 2);
  write_all(sock, file_name, file_name_len);

  /* Send file data */
  for (;;) {
    ssize_t n = read(in, buf, sizeof(buf));
    if (n > 0) {
      write_all(sock, buf, (size_t)n);
    } else if (n == 0) {
      break;
    } else {
      if (errno == EINTR) {
        continue;
      }
      perror("read");
      break;
    }
  }

  clock_t end = clock();
  double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

  printf("%lf\n", elapsed);

  close(in);

  return 0;

CLOSE_SOCK:
  close_socket(sock);
  return 1;
}
