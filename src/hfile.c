#include "cli.h"
#include "net.h"
#include "shutdown.h"
#include "server.h"
#include "client.h"

#include <stdio.h>
#include <stdlib.h>

static inline void init_server_opt(const Opt *opt, server_opt_t *server_opt) {
  server_opt->path = opt->path;
  server_opt->http_bind = opt->http_bind;
  server_opt->port = opt->port;
  server_opt->http_port = opt->http_port;
}

static inline void init_client_opt(const Opt *opt, client_opt_t *client_opt) {
  client_opt->path = opt->path;
  client_opt->message = opt->message;
  client_opt->ip = opt->ip;
  client_opt->port = opt->port;
  client_opt->msg_type = opt->msg_type;
  client_opt->msg_flags = opt->msg_flags;
}

int main(int argc, char **argv) {
  int ret = 1;

#ifdef _WIN32
  int win_argc = 0;
  char **win_argv = NULL;
  if (load_windows_utf8_argv(&win_argc, &win_argv) != 0) {
    fprintf(stderr, "failed to read command line arguments\n");
    return 1;
  }
  argc = win_argc;
  argv = win_argv;
#endif

  if (net_init() != 0) goto CLEAN_UP;
  if (shutdown_init() != 0) {
    fprintf(stderr, "failed to initialize shutdown handler\n");
    goto CLEAN_UP;
  }

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
    server_opt_t server_opt = {0};
    init_server_opt(&opt, &server_opt);
    ret = server(&server_opt);
  } else if (opt.mode == client_mode) {
    client_opt_t client_opt = {0};
    init_client_opt(&opt, &client_opt);
    ret = client(&client_opt);
  } else usage(argv[0]);

  if (shutdown_requested()) {
    ret = shutdown_exit_code();
  }

CLEAN_UP:
  shutdown_cleanup();
  net_cleanup();

#ifdef _WIN32
  free_windows_argv(win_argv, win_argc);
#endif

  return ret;
}
