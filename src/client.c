#include "helper.h"


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
  

  const char *file_name;
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

  char buf[CHUNK_SIZE];
  size_t offset = 0;
  uint16_t net_len = htons(file_name_len);
  memcpy(buf, &net_len, 2);
  offset += 2;
  memcpy(buf + offset, file_name, file_name_len);
  offset += file_name_len;

  
  write_all(sock, &net_len, 2);
  write_all(sock, file_name, file_name_len);
  
  for (;;) {
    ssize_t n = read(in, buf, sizeof(buf));
    if (n > 0) write_all(sock, buf, (size_t)n);
    else if (n == 0) break;
    else {
      if (errno == EINTR) continue;
      perror("read");
      break;
    }
  }
  

  close(in);

  return 0;

CLOSE_SOCK:
  close(sock);
  return 1; 
}

