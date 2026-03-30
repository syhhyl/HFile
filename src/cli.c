#include "cli.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

void free_windows_argv(char **argv, int argc) {
  if (argv == NULL) {
    return;
  }

  for (int i = 0; i < argc; i++) {
    free(argv[i]);
  }
  free(argv);
}

int load_windows_utf8_argv(int *argc_out, char ***argv_out) {
  int argc = 0;
  wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (wargv == NULL) {
    return 1;
  }

  char **argv = (char **)calloc((size_t)argc + 1, sizeof(*argv));
  if (argv == NULL) {
    LocalFree(wargv);
    return 1;
  }

  for (int i = 0; i < argc; i++) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
    if (size <= 0) {
      free_windows_argv(argv, argc);
      LocalFree(wargv);
      return 1;
    }

    argv[i] = (char *)malloc((size_t)size);
    if (argv[i] == NULL) {
      free_windows_argv(argv, argc);
      LocalFree(wargv);
      return 1;
    }

    if (WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], size, NULL, NULL) <= 0) {
      free_windows_argv(argv, argc);
      LocalFree(wargv);
      return 1;
    }
  }

  LocalFree(wargv);
  *argc_out = argc;
  *argv_out = argv;
  return 0;
}
#endif

void usage(const char *argv0) {
  fprintf(stderr,
          "usage:\n"
          "  %s -s <server_path> [-p <port>]\n"
          "  %s -d <server_path> [-p <port>]\n"
          "  %s -c <file_path> [-i <ip>] [-p <port>]\n"
          "  %s -m <message> [-i <ip>] [-p <port>]\n"
          "  %s status\n"
          "  %s stop\n"
          "  %s -q\n",
          argv0, argv0, argv0, argv0, argv0, argv0, argv0);
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
  opt->port = 8888;
  opt->msg_type = 0;
  opt->daemonize = 0;

  int seen_s = 0;
  int seen_d = 0;
  int seen_c = 0;
  int seen_m = 0;
  int ip_set = 0;
  int control_mode_selected = 0;
  int mode_count = 0;
  int arg_start = 1;

  if (argc > 1) {
    if (strcmp(argv[1], "status") == 0) {
      opt->mode = status_mode;
      control_mode_selected = 1;
      arg_start = 2;
    } else if (strcmp(argv[1], "stop") == 0) {
      opt->mode = stop_mode;
      control_mode_selected = 1;
      arg_start = 2;
    } else if (strcmp(argv[1], "-q") == 0) {
      opt->mode = qr_mode;
      control_mode_selected = 1;
      arg_start = 2;
    }
  }

  for (int i = arg_start; i < argc; i++) {
    const char *a = argv[i];
    if (a == NULL || a[0] != '-' || a[1] == '\0') {
      fprintf(stderr, "invalid argument\n");
      return PARSE_ERR;
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
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -s\n");
          return PARSE_ERR;
        }
        if (seen_s) {
          fprintf(stderr, "duplicate -s\n");
          return PARSE_ERR;
        }
        if (need_value(argc, argv, &i, &v) != 0 || v[0] == '-') {
          fprintf(stderr, "invalid server path\n");
          return PARSE_ERR;
        }

        opt->path = v;
        opt->mode = server_mode;
        opt->daemonize = 0;
        seen_s = 1;
        break;
      }

      case 'd': {
        const char *v = NULL;
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -d\n");
          return PARSE_ERR;
        }
        if (seen_d) {
          fprintf(stderr, "duplicate -d\n");
          return PARSE_ERR;
        }
        if (need_value(argc, argv, &i, &v) != 0 || v[0] == '-') {
          fprintf(stderr, "invalid server path\n");
          return PARSE_ERR;
        }

        opt->path = v;
        opt->mode = server_mode;
        opt->daemonize = 1;
        seen_d = 1;
        break;
      }

      case 'c': {
        const char *v = NULL;
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -c\n");
          return PARSE_ERR;
        }
        if (seen_c) {
          fprintf(stderr, "duplicate -c\n");
          return PARSE_ERR;
        }
        if (need_value(argc, argv, &i, &v) != 0 || v[0] == '-') {
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
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -m\n");
          return PARSE_ERR;
        }
        if (seen_m) {
          fprintf(stderr, "duplicate -m\n");
          return PARSE_ERR;
        }
        if (need_value(argc, argv, &i, &v) != 0 || v[0] == '-') {
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
        if (need_value(argc, argv, &i, &v) != 0 || v[0] == '-') {
          fprintf(stderr, "invalid argument\n");
          return PARSE_ERR;
        }

        opt->ip = v;
        ip_set = 1;
        break;
      }

      case 'p': {
        const char *v = NULL;
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -p\n");
          return PARSE_ERR;
        }
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

  mode_count = seen_s + seen_d + seen_c + seen_m + (control_mode_selected ? 1 : 0);

  if (mode_count == 0) {
    fprintf(stderr, "must specify one of -s, -d, -c, -m, status, stop, or -q\n");
    return PARSE_ERR;
  }

  if (seen_s && seen_d) {
    fprintf(stderr, "cannot use -s -d together\n");
    return PARSE_ERR;
  }

  if ((seen_s || seen_d) && seen_c) {
    fprintf(stderr, "cannot use server and client modes together\n");
    return PARSE_ERR;
  }

  if ((seen_s || seen_d) && seen_m) {
    fprintf(stderr, "cannot use server and client modes together\n");
    return PARSE_ERR;
  }

  if (seen_c && seen_m) {
    fprintf(stderr, "cannot use -c -m together\n");
    return PARSE_ERR;
  }

  if (!control_mode_selected) {
    opt->mode = (seen_s || seen_d) ? server_mode : client_mode;
  }
  if (opt->mode == server_mode && opt->path == NULL) return PARSE_ERR;
  if (opt->mode == client_mode && seen_c && opt->path == NULL) return PARSE_ERR;
  if (opt->mode == client_mode && seen_m && opt->message == NULL) {
    return PARSE_ERR;
  }
  if (ip_set && opt->mode != client_mode) {
    fprintf(stderr, "%s mode does not accept -i\n",
            opt->mode == server_mode ? "server" : "control");
    return PARSE_ERR;
  }

  return PARSE_OK;
}
