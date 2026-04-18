#include "cli.h"
#include "net.h"
#include "shutdown.h"
#include "server.h"
#include "client.h"
#include "control.h"

#include <stdio.h>
#include <stdlib.h>

static inline void init_server_opt(const Opt *opt, server_opt_t *server_opt) {
  server_opt->path = opt->path;
  server_opt->port = opt->port;
  server_opt->daemonize = opt->daemonize;
}

static inline void init_client_opt(const Opt *opt, client_opt_t *client_opt) {
  client_opt->path = opt->path;
  client_opt->remote_path = opt->remote_path;
  client_opt->output_path = opt->output_path;
  client_opt->message = opt->message;
  client_opt->ip = opt->ip;
  client_opt->port = opt->port;
  client_opt->msg_type = opt->msg_type;
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
  
  WSADATA wsa;
  if (WSAStartup(MAKELANGID(2, 2), &wsa) != 0) { 
    fprintf(stderr, "WSAStartup failed\n");
    goto CLEAN_UP;
  }
  
#endif

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
  } else if (opt.mode == status_mode) {
    ret = control_status();
  } else if (opt.mode == stop_mode) {
    ret = control_stop();
  } else if (opt.mode == qr_mode) {
    ret = control_print_qr();
  } else usage(argv[0]);

  if (shutdown_requested()) {
    ret = shutdown_exit_code();
  }

CLEAN_UP:
  shutdown_cleanup();

#ifdef _WIN32
  WSACleanup();
  free_windows_argv(win_argv, win_argc);
#endif

  return ret;
}
