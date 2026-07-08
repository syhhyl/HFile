#include "node.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

  int i = 2;
  if (argv[i] == NULL) return cli_error(args, "invalid argument");

  if (strcmp(argv[i], "-p") == 0) {
    i++;
    if (i >= argc) return cli_error(args, "invalid port");
    if (parse_port_arg(args, argv[i]) != 0) return 1;
    i++;
    if (i != argc) return cli_error(args, "unexpected extra argument");
    return 0;
  }

  if (is_flag(argv[i])) {
    if (strcmp(argv[i], "-i") == 0) {
      return cli_error(args, "recv mode does not accept -i");
    }
    return cli_error(args, "invalid argument");
  }

  args->path = argv[i++];
  if (i == argc) return 0;
  if (argv[i] == NULL) return cli_error(args, "invalid argument");

  if (strcmp(argv[i], "-p") != 0) {
    if (strcmp(argv[i], "-i") == 0) {
      return cli_error(args, "recv mode does not accept -i");
    }
    if (is_flag(argv[i])) return cli_error(args, "invalid argument");
    return cli_error(args, "unexpected extra argument");
  }

  i++;
  if (i >= argc) return cli_error(args, "invalid port");
  if (parse_port_arg(args, argv[i]) != 0) return 1;
  i++;
  if (i != argc) return cli_error(args, "unexpected extra argument");
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
  args->ip = argv[4];

  if (argc == 5) return 0;
  if (argv[5] == NULL || strcmp(argv[5], "-p") != 0) {
    return cli_error(args, "invalid argument order");
  }

  if (argc < 7) return cli_error(args, "invalid port");
  if (parse_port_arg(args, argv[6]) != 0) return 1;
  if (argc != 7) return cli_error(args, "unexpected extra argument");
  return 0;
}

static int parse_cli(int argc, char **argv, CliArgs *args) {
  memset(args, 0, sizeof(*args));
  args->mode = MODE_RECV;
  args->port = DEFAULT_PORT;

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
