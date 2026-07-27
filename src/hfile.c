#include "node.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum {
  MODE_RECV,
  MODE_SEND,
} Mode;

typedef struct {
  Mode mode;
  const char *path;
  const char *ip;
  uint16_t port;
  int show_usage;
  const char *error;
} CliArgs;

#define DEFAULT_PORT 8888

static int cli_error(CliArgs *args, const char *msg) {
  args->error = msg;
  return 1;
}

static int is_flag(const char *arg) {
  return arg != NULL && arg[0] == '-' && arg[1] != '\0';
}

static int parse_port(const char *s, uint16_t *port) {
  if (s == NULL || *s == '\0') return 1;

  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v == 0 || v > 65535UL) {
    return 1;
  }

  *port = (uint16_t)v;
  return 0;
}

static int parse_port_arg(CliArgs *args, const char *s) {
  uint16_t port;
  if (parse_port(s, &port) != 0) return cli_error(args, "invalid port");
  args->port = port;
  return 0;
}

static int parse_recv_args(int argc, char **argv, CliArgs *args) {
  args->mode = MODE_RECV;
  args->path = ".";

  if (argc == 2) return 0;

  int option_index = 2;
  if (argv[option_index] == NULL) {
    return cli_error(args, "invalid argument");
  }

  if (!is_flag(argv[option_index])) {
    args->path = argv[option_index++];
    if (option_index == argc) return 0;
    if (argv[option_index] == NULL) {
      return cli_error(args, "invalid argument");
    }
  }

  if (strcmp(argv[option_index], "-p") != 0) {
    if (strcmp(argv[option_index], "-i") == 0) {
      return cli_error(args, "recv mode does not accept -i");
    }
    if (is_flag(argv[option_index])) {
      return cli_error(args, "invalid argument");
    }
    return cli_error(args, "unexpected extra argument");
  }

  if (option_index + 1 >= argc || argv[option_index + 1] == NULL) {
    return cli_error(args, "invalid port");
  }

  int option_argc = argc - option_index + 1;
  char **option_argv = argv + option_index - 1;
  int option = getopt(option_argc, option_argv, ":i:p:");
  if (option != 'p' || parse_port_arg(args, optarg) != 0) return 1;
  if (optind != option_argc) {
    return cli_error(args, "unexpected extra argument");
  }
  return 0;
}

static int parse_send_args(int argc, char **argv, CliArgs *args) {
  args->mode = MODE_SEND;

  if (argc < 3 || argv[2] == NULL || is_flag(argv[2])) {
    return cli_error(args, "missing file to send");
  }
  args->path = argv[2];

  if (argc < 4) return cli_error(args, "missing target address");
  if (argv[3] == NULL || strcmp(argv[3], "-i") != 0) {
    return cli_error(args, "invalid argument order");
  }

  if (argc < 5 || argv[4] == NULL || argv[4][0] == '\0' || is_flag(argv[4])) {
    return cli_error(args, "missing target address");
  }

  int option_argc = argc - 2;
  char **option_argv = argv + 2;
  int option = getopt(option_argc, option_argv, ":i:p:");
  if (option != 'i' || optarg == NULL || optarg[0] == '\0' || is_flag(optarg)) {
    return cli_error(args, "missing target address");
  }
  args->ip = optarg;

  if (optind == option_argc) return 0;
  if (option_argv[optind] == NULL || strcmp(option_argv[optind], "-p") != 0) {
    return cli_error(args, "invalid argument order");
  }

  if (optind + 1 >= option_argc || option_argv[optind + 1] == NULL) {
    return cli_error(args, "invalid port");
  }

  option = getopt(option_argc, option_argv, ":i:p:");
  if (option != 'p' || parse_port_arg(args, optarg) != 0) return 1;
  if (optind != option_argc) {
    return cli_error(args, "unexpected extra argument");
  }
  return 0;
}

static int parse_cli(int argc, char **argv, CliArgs *args) {
  memset(args, 0, sizeof(*args));
  args->mode = MODE_RECV;
  args->port = DEFAULT_PORT;
  opterr = 0;
  optind = 1;

  if (argc < 2 || argv == NULL || argv[1] == NULL) {
    return cli_error(args, "missing command");
  }

  if (strcmp(argv[1], "-h") == 0) {
    args->show_usage = 1;
    return 0;
  }

  if (strcmp(argv[1], "recv") == 0) {
    return parse_recv_args(argc, argv, args);
  }

  if (strcmp(argv[1], "send") == 0) {
    return parse_send_args(argc, argv, args);
  }

  return cli_error(args, "unknown command");
}

#ifndef HF_CLI_NO_MAIN

static void print_usage(FILE *stream, const char *program) {
  fprintf(stream,
    "usage:\n"
    "  %s recv [<dir>] [-p <port>]\n"
    "  %s send <file> -i <ip> [-p <port>]\n"
    "\n"
    "options:\n"
    "  -i <ip>    target node address\n"
    "  -p <port>  port number (default 8888)\n"
    "  -h         show this help\n"
    , program, program);
}

int main(int argc, char **argv) {
  CliArgs args;
  int parse_error = parse_cli(argc, argv, &args);

  if (args.show_usage || parse_error) {
    FILE *usage_stream = args.show_usage ? stdout : stderr;
    if (args.error != NULL) fprintf(stderr, "%s\n", args.error);
    print_usage(usage_stream, argv[0]);
    return args.show_usage ? 0 : 1;
  }

  return args.mode == MODE_RECV ?
    node_recv(args.path, args.port) : node_send(args.path, args.ip, args.port);
}

#endif
