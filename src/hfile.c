#include "net.h"
#include "shutdown.h"
#include "server.h"
#include "client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  PARSE_OK,
  PARSE_HELP,
  PARSE_ERR
} parse_result_t;

typedef enum {
  MODE_SERVER,
  MODE_CLIENT,
} Mode;

typedef struct {
  Mode mode;
  const char *path;
  const char *ip;
  uint16_t port;
  int print_qr;
} Opt;

static void usage(const char *argv0) {
  fprintf(stderr,
    "HFile — fast file transfer over LAN\n"
    "\n"
    "usage:\n"
    "  %s [<path>]  [-p <port>] [-q]  start receive server\n"
    "  %s -s <file> [-i <ip>] [-p <port>]  send a file\n"
    "\n"
    "options:\n"
    "  <path>     receive directory (default .)\n"
    "  -s <file>  file to send\n"
    "  -i <ip>    server address\n"
    "  -p <port>  port number (default 8888)\n"
    "  -q         print server QR code\n"
    "  -h         show this help\n"
    "\n"
    "examples:\n"
    "  %s /tmp/receive\n"
    "  %s -s ./foo.txt\n"
    "  %s -s ./bar.bin -p 7777\n",
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
  opt->print_qr = 0;
  opt->mode = MODE_SERVER;

  int send_seen = 0;
  int ip_seen = 0;
  int positional_seen = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a == NULL) {
      fprintf(stderr, "invalid argument\n");
      return PARSE_ERR;
    }

    if (a[0] != '-' || a[1] == '\0') {
      if (send_seen) {
        fprintf(stderr, "unexpected positional argument\n");
        return PARSE_ERR;
      }
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

      case 's': {
        const char *v = NULL;
        if (send_seen) {
          fprintf(stderr, "duplicate -s\n");
          return PARSE_ERR;
        }
        if (positional_seen) {
          fprintf(stderr, "cannot use server path with -s\n");
          return PARSE_ERR;
        }
        if (take_value(argc, argv, &i, "invalid file path", &v) != 0) {
          return PARSE_ERR;
        }

        opt->mode = MODE_CLIENT;
        opt->path = v;
        send_seen = 1;
        break;
      }

      case 'i': {
        const char *v = NULL;
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

      case 'q':
        opt->print_qr = 1;
        break;

      default:
        fprintf(stderr, "invalid argument\n");
        return PARSE_ERR;
    }
  }

  if (opt->mode == MODE_SERVER && opt->path == NULL) {
    opt->path = ".";
  }

  if (opt->mode == MODE_CLIENT && opt->path == NULL) {
    fprintf(stderr, "missing file to send\n");
    return PARSE_ERR;
  }

  if (ip_seen && opt->mode != MODE_CLIENT) {
    fprintf(stderr, "server mode does not accept -i\n");
    return PARSE_ERR;
  }

  if (opt->print_qr && opt->mode != MODE_SERVER) {
    fprintf(stderr, "client mode does not accept -q\n");
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

  if (opt.mode == MODE_SERVER) {
    server_opt_t server_opt = {
      .path = opt.path,
      .port = opt.port,
      .print_qr = opt.print_qr
    };
    ret = server(&server_opt);
  } else if (opt.mode == MODE_CLIENT) {
    client_opt_t client_opt = { .path = opt.path, .ip = opt.ip, .port = opt.port };
    ret = client(&client_opt);
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
