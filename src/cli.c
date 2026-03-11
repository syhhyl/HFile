#include "cli.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"

void usage(const char *argv0) {
  fprintf(stderr,
          "usage:\n"
          "  %s -s <server_path> [-p <port>] [--perf]\n"
          "  %s -c <file_path> [-i <ip>] [-p <port>] [--perf] [--compress]\n"
          "  %s -m <message> [-i <ip>] [-p <port>] [--perf]\n",
          argv0, argv0, argv0);
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
  opt->message = NULL;
  opt->ip = "127.0.0.1";
  opt->port = 9000;
  opt->perf = 0;
  opt->compress = 0;
  opt->msg_type = 0;
  opt->msg_flags = HF_MSG_FLAG_NONE;

  int seen_s = 0;
  int seen_c = 0;
  int seen_m = 0;
  int ip_set = 0;
  int mode_count = 0;

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
        opt->message = NULL;
        opt->msg_type = HF_MSG_TYPE_FILE_TRANSFER;
        seen_c = 1;
        break;
      }
      
      case 'm': {
        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid message\n");
          return PARSE_ERR;
        }
        opt->mode = client_mode;
        opt->path = NULL;
        opt->message = v;
        opt->msg_type = HF_MSG_TYPE_TEXT_MESSAGE;
        seen_m = 1;
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
        fprintf(stderr, "invalid argument\n");
        return PARSE_ERR;
    }
  }

  mode_count = seen_s + seen_c + seen_m;

  if (mode_count == 0) {
    fprintf(stderr, "must specify one of -s, -c, or -m\n");
    return PARSE_ERR;
  }

  if (seen_s && seen_c) {
    fprintf(stderr, "cannot use -s -c together\n");
    return PARSE_ERR;
  }

  if (seen_s && seen_m) {
    fprintf(stderr, "cannot use -s -m together\n");
    return PARSE_ERR;
  }

  if (seen_c && seen_m) {
    fprintf(stderr, "cannot use -c -m together\n");
    return PARSE_ERR;
  }

  opt->mode = seen_s ? server_mode : client_mode;
  if (opt->mode == server_mode && opt->path == NULL) return PARSE_ERR;
  if (opt->mode == client_mode && seen_c && opt->path == NULL) return PARSE_ERR;
  if (opt->mode == client_mode && seen_m && opt->message == NULL) {
    return PARSE_ERR;
  }
  if (ip_set && opt->mode != client_mode) {
    fprintf(stderr, "server mode does not accept -i\n");
    return PARSE_ERR;
  }

  if (seen_m && opt->compress) {
    fprintf(stderr, "message mode does not accept --compress\n");
    return PARSE_ERR;
  }

  return PARSE_OK;
}
