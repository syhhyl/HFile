#include "cli.h"
#include "net.h"
#include "server.h"
#include "client.h"

int main(int argc, char **argv) {
  
  net_init();

  Opt opt;
  parse_result_t res = parse_args(argc, argv, &opt);

  if (res == PARSE_HELP) {
    usage(argv[0]);
    net_cleanup();
    return 0;
  }

  if (res != PARSE_OK) {
    usage(argv[0]);
    net_cleanup();
    return 1;
  }

  int ret = 1;
  if (opt.mode == server_mode) {
    ret = server(opt.path, opt.port, opt.perf);
  } else if (opt.mode == client_mode) {
    ret = client(opt.path, opt.ip, opt.port, opt.perf);
  } else usage(argv[0]);

  net_cleanup();
  return ret;
}
