#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include "server.h"
#include "client.h"
#include "helper.h"


typedef enum {
  server_mode,
  client_mode,
  init_mode
} state;

state flag = init_mode;

int main(int argc, char *argv[]) {
  int opt;
  opt = getopt(argc, argv, "s:c:");
  if (opt != '?') {
    DBG("opt=%c", opt);
    if (opt == 's') {
      flag = server_mode;
      printf("server mode\n");
      printf("optarg:%s\n", optarg);
      server(optarg);
      
      
    } else if (opt == 'c') {
      flag = client_mode;
      printf("client mode\n");
      printf("optarg:%s\n", optarg);
      client(optarg);
    }
  } else {
    DBG("opt=%c", opt);
  }
  return 0;
}
