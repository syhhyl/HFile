#include "cli.h"
#include "net.h"
#include "server.h"
#include "client.h"

int main(int argc, char **argv) {
  
  net_init();

  Opt opt = {0};
  parse_result_t res = parse_args(argc, argv, &opt);

  int ret;

  if (res == PARSE_HELP) {
    usage(argv[0]);
    net_cleanup();
    ret = 0;
  }

  if (res != PARSE_OK) {
    usage(argv[0]);
    net_cleanup();
    ret = 1;
  }

  if (opt.mode == server_mode) {
    ret = server(opt.path, opt.port, opt.perf);
  } else if (opt.mode == client_mode) {
    ret = client(opt.path, opt.ip, opt.port, opt.perf);
  } else usage(argv[0]);

  net_cleanup();
  return ret;
}
