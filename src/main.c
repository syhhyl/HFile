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

  Opt opt;
  parse_args(argc, argv, &opt);

  if (opt.exit_code == 1) {
    usage(argv[0]);
    return 1;
  }

  // DBG("path: %s port: %u", path, port);
  if (opt.mode == server_mode)
    return server(opt.path, opt.port);
  else if (opt.mode == client_mode)
    return client(opt.path, opt.ip, opt.port);
      
}
