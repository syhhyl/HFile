#include "helper.h"

int main(int argc, char **argv) {

#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }
#endif

  Opt opt;
  parse_result_t res = parse_args(argc, argv, &opt);

  if (res == PARSE_HELP) {
    usage(argv[0]);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
  }

  if (res != PARSE_OK) {
    usage(argv[0]);
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  }

  int ret = 1;
  if (opt.mode == server_mode) {
    ret = server(opt.path, opt.port);
  } else if (opt.mode == client_mode) {
    ret = client(opt.path, opt.ip, opt.port);
  } else usage(argv[0]);

#ifdef _WIN32
  WSACleanup();
#endif
  return ret;
}
