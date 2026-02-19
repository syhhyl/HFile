#include "helper.h"

int main(int argc, char **argv) {
  Opt opt;
  parse_result_t res = parse_args(argc, argv, &opt);

  if (res == PARSE_HELP) {
    usage(argv[0]);
    return 0;
  }

  if (res != PARSE_OK) {
    usage(argv[0]);
    return 1;
  }

  if (opt.mode == server_mode) {
    return server(opt.path, opt.port);
  }
  if (opt.mode == client_mode) {
    return client(opt.path, opt.ip, opt.port);
  }

  usage(argv[0]);
  return 1;
}
