#include <sys/socket.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>

int main() {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);  
  if (listen_fd < 0) {
    perror("socket");
    return 1;
  }
  
  struct sockaddr_in addr;
  //memset()
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9000);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listen_fd, (struct sockaddr *)&addr,
      sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  
  listen(listen_fd, 1);
  
  //TODO accepts more connection
  int conn_flag = true;
  while (conn_flag) {
    printf("listening on port 9000...\n");
    int conn = accept(listen_fd, NULL, NULL);
    printf("client connected\n");

    int out = open("out.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    char buf[4096];
    while (1) {
      ssize_t n = read(conn, buf, sizeof(buf));
      if (n <= 0) break;
      write(out, buf, n);
    }

    close(conn);
  }

  close(listen_fd);
  
  return 0;

  
}