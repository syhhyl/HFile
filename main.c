#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include "server.h"
#include "client.h"
#include "stdint.h"
#include "stdlib.h"
#include "helper.h"


typedef enum {
  server_mode,
  client_mode,
  init_mode
} state;

state flag = init_mode;

const char *ip = "127.0.0.1";
uint16_t port = 9000;
char *file_path = NULL;

int main(int argc, char *argv[]) {
  int opt;
  while ((opt = getopt(argc, argv, "s:c:i:p:")) != -1) {
    if (opt == 's') {
      flag = server_mode;
      printf("server mode\n");
      printf("optarg:%s\n", optarg);
      server(optarg);
      return 0;
    } else if (opt == 'c') {
      flag = client_mode;
      printf("client mode\n");
      printf("optarg:%s\n", optarg);
      file_path = optarg;
    } else if (opt == 'i') {
      ip = optarg;
    } else if (opt == 'p') {
      port = (uint16_t)atoi(optarg);
    } else {
      DBG("opt=%c", opt);
    }
  }
  if (flag == client_mode) {
    if (file_path == NULL) {
      fprintf(stderr, "missing -c <file_path>\n");
      return 1;
    }
  }
  client(file_path, ip, port);
  return 0;
}
