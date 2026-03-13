#include "cli.h"
#include "net.h"
#include "server.h"
#include "client.h"

static inline void init_server_opt(Opt *opt, server_opt_t *server_opt) {
  server_opt->path = opt->path;
  server_opt->port = opt->port;
  server_opt->perf = opt->perf;
}

static inline void init_client_opt(Opt *opt, client_opt_t *client_opt) {
  client_opt->path = opt->path;
  client_opt->message = opt->message;
  client_opt->ip = opt->ip;
  client_opt->port = opt->port;
  client_opt->perf = opt->perf;
  client_opt->msg_type = opt->msg_type;
  client_opt->msg_flags = opt->msg_flags;
}

int main(int argc, char **argv) {
  net_init();

  int ret = 1;
  Opt opt = {0};
  parse_result_t res = parse_args(argc, argv, &opt);

  if (res == PARSE_HELP) {
    usage(argv[0]);
    ret = 0;
    goto CLEAN_UP;
  } else if (res == PARSE_ERR) {
    usage(argv[0]);
    ret = 1;
    goto CLEAN_UP;
  }

  if (opt.mode == server_mode) {
    server_opt_t server_opt;
    init_server_opt(&opt, &server_opt);
    ret = server(&server_opt);
  } else if (opt.mode == client_mode) {
    client_opt_t client_opt;
    init_client_opt(&opt, &client_opt);
    ret = client(&client_opt);
  } else usage(argv[0]);

CLEAN_UP:
  net_cleanup();
  return ret;
}
