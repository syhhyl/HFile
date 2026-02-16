#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include "server.h"
#include "client.h"
#include "stdint.h"
#include "stdlib.h"
#include "helper.h"
#include "stdbool.h"



int main(int argc, char **argv) {
  int opt;
  
  Mode st = init_mode;
  char *path = NULL;
  char *ip = "127.0.0.1";
  uint16_t port = 9000;
  

  while ((opt = getopt(argc, argv, "s:c:i:p:h")) != -1) { 
    switch (opt) {
      case 's':
        if (st == client_mode) {
          fprintf(stderr, "cannot use -s -c together\n");
          goto BAD_USE;
        }
        st = server_mode;
        if (*optarg == '-') {
          fprintf(stderr, "invalid server path\n");
          goto BAD_USE;
        }
        path = optarg;
        break;
      case 'c':
        if (st == server_mode) {
          fprintf(stderr, "cannot use -s -c together\n");
          goto BAD_USE;
        }
        st = client_mode;
        if (*optarg == '-') {
          fprintf(stderr, "invalid client path\n");
          goto BAD_USE;
        }
        path = optarg;
        break;
      case 'i':
        if (st == client_mode)
          ip = optarg;
        else {
          fprintf(stderr, "server mode don't need ip\n");
          goto BAD_USE;
        }
        break;
      case 'p':
        if (parse_port(optarg, &port) != 0) {
          fprintf(stderr, "invalid port: %s\n", optarg);
          goto BAD_USE;
        }
        break;
      case 'h':
        usage(argv[0]);
        return 0;
    }
  }

  // DBG("path: %s port: %u", path, port);
  if (st == server_mode)
    return server(path, port);
  else if (st == client_mode)
    return client(path, ip, port);
      

BAD_USE:
  return 1;
  
}
