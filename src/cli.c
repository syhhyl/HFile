#include "cli.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void usage(const char *argv0) {
  fprintf(stderr,
          "usage:\n"
          "  %s -s <server_path> [-p <port>] [--perf]\n"
          "  %s -c <file_path> [-i <ip>] [-p <port>] [--perf] [--compress]\n",
          argv0, argv0);
}

int parse_port(const char *s, uint16_t *out) {
  if (s == NULL || *s == '\0') return 1;
  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return 1;
  if (v == 0 || v > 65535UL) return 1;
  *out = (uint16_t)v;
  return 0;
}

int need_value(int argc, char **argv, int *i, const char **out) {
  if (*i + 1 >= argc) return 1;
  *i = *i + 1;
  *out = argv[*i];
  return 0;
}

parse_result_t parse_args(int argc, char **argv, Opt *opt) {
  if (opt == NULL) return PARSE_ERR;

  opt->mode = init_mode;
  opt->path = NULL;
  opt->ip = "127.0.0.1";
  opt->port = 9000;
  opt->perf = 0;
  opt->compress = 0;

  int seen_s = 0;
  int seen_c = 0;
  int ip_set = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a == NULL || a[0] != '-' || a[1] == '\0') {
      fprintf(stderr, "invalid argument\n");
      return PARSE_ERR;
    }

    if (a[1] == '-') {
      if (strcmp(a, "--perf") == 0) {
        opt->perf = 1;
        continue;
      } else if (strcmp(a, "--compress") == 0) {
        opt->compress = 1;
        continue;
      }
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
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid server path\n");
          return PARSE_ERR;
        }
        if (v[0] == '-') {
          fprintf(stderr, "invalid server path\n");
          return PARSE_ERR;
        }

        opt->mode = server_mode;
        opt->path = v;
        seen_s = 1;
        break;
      }

      case 'c': {
        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid client path\n");
          return PARSE_ERR;
        }
        if (v[0] == '-') {
          fprintf(stderr, "invalid client path\n");
          return PARSE_ERR;
        }

        opt->mode = client_mode;
        opt->path = v;
        seen_c = 1;
        break;
      }

      case 'i': {
        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid argument\n");
          return PARSE_ERR;
        }
        if (v[0] == '-') {
          fprintf(stderr, "invalid argument\n");
          return PARSE_ERR;
        }

        opt->ip = v;
        ip_set = 1;
        break;
      }

      case 'p': {
        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid port\n");
          return PARSE_ERR;
        }
        if (parse_port(v, &opt->port) != 0) {
          fprintf(stderr, "invalid port\n");
          return PARSE_ERR;
        }
        break;
      }

      default:
        return PARSE_ERR;
    }
  }

  if (seen_s && seen_c) {
    fprintf(stderr, "cannot use -s -c together\n");
    return PARSE_ERR;
  }

  if (!seen_s && !seen_c) {
    if (ip_set) {
      fprintf(stderr, "server mode don't need ip\n");
    }
    return PARSE_ERR;
  }

  opt->mode = seen_s ? server_mode : client_mode;
  if (opt->path == NULL) return PARSE_ERR;
  if (ip_set && opt->mode != client_mode) {
    fprintf(stderr, "server mode don't need ip\n");
    return PARSE_ERR;
  }

  return PARSE_OK;
}
