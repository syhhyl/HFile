#include <stdio.h>
#include <unistd.h>
#include <getopt.h>

typedef enum {
  server,
  client,
  init
} state;

state flag = init;

int main(int argc, char *argv[]) {
  int opt;
  opt = getopt(argc, argv, "sc:");
  if (opt != -1) {
    if (opt == 's') {
      flag = server;
      printf("server mode\n");
    } else if (opt == 'c') {
      flag = client;
      printf("client mode\n");
      
      printf("optarg:%s\n", optarg);
    }
  }
  return 0;
}
