#include "node.h"
#include "shutdown.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  PARSE_OK,
  PARSE_HELP,
  PARSE_ERR
} parse_result_t;

typedef enum {
  MODE_RECV,
  MODE_SEND,
} Mode;

typedef struct {
  Mode mode;
  const char *path;
  const char *ip;
  uint16_t port;
} Opt;

static void usage(const char *argv0) {
  fprintf(stderr,
    "HFile - fast file transfer over LAN\n"
    "\n"
    "usage:\n"
    "  %s recv [<dir>] [-p <port>]\n"
    "  %s send <file> [-i <ip>] [-p <port>]\n"
    "\n"
    "options:\n"
    "  -i <ip>    target node address\n"
    "  -p <port>  port number (default 8888)\n"
    "  -h         show this help\n"
    "\n"
    "examples:\n"
    "  %s recv /tmp/receive\n"
    "  %s send ./foo.txt\n"
    "  %s send ./bar.bin -i 192.168.1.10 -p 7777\n",
    argv0, argv0, argv0, argv0, argv0);
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

static int need_value(int argc, char **argv, int *i, const char **out) {
  if (*i + 1 >= argc) return 1;
  *i = *i + 1;
  *out = argv[*i];
  return 0;
}

static int take_value(int argc, char **argv, int *i,
                      const char *error_message, const char **out) {
  if (need_value(argc, argv, i, out) != 0) {
    fprintf(stderr, "%s\n", error_message);
    return 1;
  }
  return 0;
}

static parse_result_t parse_args(int argc, char **argv, Opt *opt) {
  if (opt == NULL) return PARSE_ERR;

  opt->path = NULL;
  opt->ip = NULL;
  opt->port = 8888;
  opt->mode = MODE_RECV;

  int ip_seen = 0;
  int positional_seen = 0;

  if (argc < 2 || argv[1] == NULL) {
    fprintf(stderr, "missing command\n");
    return PARSE_ERR;
  }

  if (strcmp(argv[1], "-h") == 0) {
    return PARSE_HELP;
  }

  if (strcmp(argv[1], "recv") == 0) {
    opt->mode = MODE_RECV;
  } else if (strcmp(argv[1], "send") == 0) {
    opt->mode = MODE_SEND;
  } else {
    fprintf(stderr, "unknown command\n");
    return PARSE_ERR;
  }

  for (int i = 2; i < argc; i++) {
    const char *a = argv[i];
    if (a == NULL) {
      fprintf(stderr, "invalid argument\n");
      return PARSE_ERR;
    }

    if (a[0] != '-' || a[1] == '\0') {
      if (positional_seen) {
        fprintf(stderr, "unexpected extra argument\n");
        return PARSE_ERR;
      }
      opt->path = a;
      positional_seen = 1;
      continue;
    }

    if (a[1] == '-') {
      fprintf(stderr, "invalid argument\n");
      return PARSE_ERR;
    }

    if (a[2] != '\0') {
      fprintf(stderr, "invalid argument\n");
      return PARSE_ERR;
    }

    switch (a[1]) {
      case 'h':
        return PARSE_HELP;

      case 'i': {
        const char *v = NULL;
        if (opt->mode == MODE_RECV) {
          fprintf(stderr, "recv mode does not accept -i\n");
          return PARSE_ERR;
        }
        if (take_value(argc, argv, &i, "invalid address", &v) != 0) {
          return PARSE_ERR;
        }

        opt->ip = v;
        ip_seen = 1;
        break;
      }

      case 'p': {
        const char *v = NULL;
        if (take_value(argc, argv, &i, "invalid port", &v) != 0) {
          return PARSE_ERR;
        }
        if (parse_port(v, &opt->port) != 0) {
          fprintf(stderr, "invalid port\n");
          return PARSE_ERR;
        }
        break;
      }

      default:
        fprintf(stderr, "invalid argument\n");
        return PARSE_ERR;
    }
  }

  if (opt->mode == MODE_RECV && opt->path == NULL) {
    opt->path = ".";
  }

  if (opt->mode == MODE_SEND && opt->path == NULL) {
    fprintf(stderr, "missing file to send\n");
    return PARSE_ERR;
  }

  if (ip_seen && opt->mode != MODE_SEND) {
    fprintf(stderr, "recv mode does not accept -i\n");
    return PARSE_ERR;
  }

  return PARSE_OK;
}

int main(int argc, char **argv) {
  int ret = 1;

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

  if (opt.mode == MODE_RECV) {
    ret = node_recv(opt.path, opt.port);
  } else if (opt.mode == MODE_SEND) {
    ret = node_send(opt.path, opt.ip, opt.port);
  } else {
    usage(argv[0]);
    ret = 1;
  }

  if (shutdown_signal_number() != 0) {
    ret = shutdown_exit_code();
  }

CLEAN_UP:
  shutdown_cleanup();

  return ret;
}
