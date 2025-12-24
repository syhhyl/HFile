#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

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
  char *file_path = argv[1];
  int in = open(file_path, O_RDONLY);
  char buf[4096];
  while (1) {
    ssize_t n = read(in, buf, sizeof(buf));
    if (n <= 0) break;
    write(sock, buf, n);
  }
}