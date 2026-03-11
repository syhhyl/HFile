#include "cli.h"
#include "net.h"
#include "server.h"
#include "client.h"

int main(int argc, char **argv) {
  net_init();

  int ret = 1;
  Opt opt = {0};
  parse_result_t res = parse_args(argc, argv, &opt);


  if (res == PARSE_HELP) {
    usage(argv[0]);
    net_cleanup();
    return 0;
  } else if (res == PARSE_ERR) {
    usage(argv[0]);
    net_cleanup();
    return 1;
  }

  if (opt.mode == server_mode) {
    server_opt_t ser_opt = {
      .path = opt.path,
      .port = opt.port,
      .perf = opt.perf
    };
    ret = server(&ser_opt);
  } else if (opt.mode == client_mode) {
    client_opt_t cli_opt = {
      .path = opt.path,
      .message = opt.message,
      .ip = opt.ip,
      .port = opt.port,
      .perf = opt.perf,
      .msg_type = opt.msg_type,
      .msg_flags = opt.msg_flags
    };
    ret = client(&cli_opt);
  } else {
    usage(argv[0]);
  }

  net_cleanup();
  return ret;
}
