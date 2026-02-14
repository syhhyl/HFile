#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include "server.h"
#include "client.h"
#include "stdint.h"
#include "stdlib.h"
#include "helper.h"
#include "stdbool.h"

typedef enum {
  server_mode,
  client_mode,
  init_mode
} state;

state flag = init_mode;

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage:\n"
          "  %s -s <server_path> [-p <port>]\n"
          "  %s -c <file_path> [-i <ip>] [-p <port>]\n",
          argv0, argv0);
}

static int parse_port(const char *s, uint16_t *out) {
  if (s == NULL || *s == '\0') return 1;
  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return 1;
  if (v == 0 || v > 65535UL) return 1;
  *out = (uint16_t)v;
  return 0;
}


int main(int argc, char **argv) {
  int opt;
  
  state st = init_mode;
  char *path = NULL;
  char *ip = "127.0.0.1";
  uint16_t port = 9000;
  

  while ((opt = getopt(argc, argv, "s:c:i:p:h")) != -1) { 
    switch (opt) {
      case 's':
        if (st == client_mode) {
          fprintf(stderr, "cannot use -c and -s together\n");
          goto BAD_USE;
        }
        st = server_mode;
        if (*optarg == '-') goto BAD_USE;
        path = optarg;
        break;
      case 'c':
        if (st == server_mode) {
          fprintf(stderr, "cannot use -s and -c together\n");
          goto BAD_USE;
        }
        st = client_mode;
        if (*optarg == '-') goto BAD_USE;
        path = optarg;
        break;
      case 'i':
        if (st == client_mode)
          ip = optarg;
        else goto BAD_USE;
        break;
      case 'p':
        if (parse_port(optarg, &port) != 0) {
          fprintf(stderr, "invalid port: %s\n", optarg);
          goto BAD_USE;
        }
        break;
      case 'h':
        usage(argv[0]);
        return 0;
    }
  }

  DBG("path: %s port: %u", path, port);
  if (st == server_mode)
    return server(path, port);
  else if (st == client_mode)
    return client(path, ip, port);
      

BAD_USE:
  usage(argv[0]);
  return 1;
  
}
