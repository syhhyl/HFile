/**
 * @file main.c
 * @brief Main entry point for HFile
 */

#include "helper.h"

#ifdef _WIN32
#include <stdio.h>
#endif

/**
 * @brief Main entry point
 * @param argc Argument count
 * @param argv Argument vector
 * @return Exit code
 */
int main(int argc, char **argv) {
#ifdef _WIN32
  /* Initialize Windows Sockets on Windows */
  if (winsock_init() != 0) {
    fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }
#endif

  Opt opt;
  parse_result_t res = parse_args(argc, argv, &opt);

  if (res == PARSE_HELP) {
    usage(argv[0]);
#ifdef _WIN32
    winsock_cleanup();
#endif
    return 0;
  }

  if (res != PARSE_OK) {
    usage(argv[0]);
#ifdef _WIN32
    winsock_cleanup();
#endif
    return 1;
  }

  int ret = 0;
  if (opt.mode == server_mode) {
    ret = server(opt.path, opt.port);
  } else if (opt.mode == client_mode) {
    ret = client(opt.path, opt.ip, opt.port);
  } else {
    usage(argv[0]);
    ret = 1;
  }

#ifdef _WIN32
  winsock_cleanup();
#endif
  return ret;
}
