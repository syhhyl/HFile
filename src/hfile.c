#include "cli.h"
#include "net.h"
#include "shutdown.h"
#include "server.h"
#include "client.h"

#include <stdio.h>
#include <stdlib.h>

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

  if (res == PARSE_HELP || res == PARSE_ERR) {
    usage(argv[0]);
    ret = (res == PARSE_HELP) ? 0 : 1;
    goto CLEAN_UP;
  }

  if (opt.mode == server_mode) {
    server_opt_t server_opt = { .path = opt.path, .port = opt.port };
    ret = server(&server_opt);
  } else if (opt.mode == client_mode) {
    client_opt_t client_opt = { .path = opt.path, .ip = opt.ip, .port = opt.port };
    ret = client(&client_opt);
  } else usage(argv[0]);

  if (shutdown_signal_number() != 0) {
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
