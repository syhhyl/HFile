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
          "  %s -d <server_path> [-p <port>]\n"
          "  %s -c <file_path> [-i <ip>] [-p <port>]\n"
          "  %s -g <remote_file> [-o <local_path>] [-i <ip>] [-p <port>]\n"
          "  %s -m <message> [-i <ip>] [-p <port>]\n"
          "  %s status\n"
          "  %s stop\n",
          argv0, argv0, argv0, argv0, argv0, argv0);
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

parse_result_t parse_args(int argc, char **argv, Opt *opt) {
  if (opt == NULL) return PARSE_ERR;

  opt->path = NULL;
  opt->remote_path = NULL;
  opt->output_path = NULL;
  opt->message = NULL;
  opt->ip = "127.0.0.1";
  opt->port = 8888;
  opt->msg_type = 0;

  int server_selected = 0;
  int client_actions = 0;
  char client_action = '\0';
  int output_seen = 0;
  int ip_seen = 0;
  int control_mode_selected = 0;
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

      case 'd': {
        const char *v = NULL;
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -d\n");
          return PARSE_ERR;
        }
        if (server_selected) {
          fprintf(stderr, "duplicate -d\n");
          return PARSE_ERR;
        }
        if (take_value(argc, argv, &i, "invalid server path", &v) != 0) {
          return PARSE_ERR;
        }

        opt->path = v;
        opt->mode = server_mode;
        server_selected = 1;
        break;
      }

      case 'c': {
        const char *v = NULL;
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -c\n");
          return PARSE_ERR;
        }
        if (client_action == 'c') {
          fprintf(stderr, "duplicate -c\n");
          return PARSE_ERR;
        }
        if (take_value(argc, argv, &i, "invalid client path", &v) != 0) {
          return PARSE_ERR;
        }

        opt->mode = client_mode;
        opt->path = v;
        opt->message = NULL;
        opt->msg_type = HF_MSG_TYPE_SEND_FILE;
        client_action = 'c';
        client_actions++;
        break;
      }

      case 'g': {
        const char *v = NULL;
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -g\n");
          return PARSE_ERR;
        }
        if (client_action == 'g') {
          fprintf(stderr, "duplicate -g\n");
          return PARSE_ERR;
        }
        if (take_value(argc, argv, &i, "invalid remote file", &v) != 0) {
          return PARSE_ERR;
        }

        opt->mode = client_mode;
        opt->remote_path = v;
        opt->msg_type = HF_MSG_TYPE_GET_FILE;
        client_action = 'g';
        client_actions++;
        break;
      }
      
      case 'm': {
        const char *v = NULL;
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -m\n");
          return PARSE_ERR;
        }
        if (client_action == 'm') {
          fprintf(stderr, "duplicate -m\n");
          return PARSE_ERR;
        }
        if (take_value(argc, argv, &i, "invalid message", &v) != 0) {
          return PARSE_ERR;
        }
        
        opt->mode = client_mode;
        opt->path = NULL;
        opt->message = v;
        opt->msg_type = HF_MSG_TYPE_TEXT_MESSAGE;
        client_action = 'm';
        client_actions++;
        break;
      }

      case 'o': {
        const char *v = NULL;
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -o\n");
          return PARSE_ERR;
        }
        if (output_seen) {
          fprintf(stderr, "duplicate -o\n");
          return PARSE_ERR;
        }
        if (take_value(argc, argv, &i, "invalid output path", &v) != 0) {
          return PARSE_ERR;
        }

        opt->output_path = v;
        output_seen = 1;
        break;
      }

      case 'i': {
        const char *v = NULL;
        if (take_value(argc, argv, &i, "invalid argument", &v) != 0) {
          return PARSE_ERR;
        }

        opt->ip = v;
        ip_seen = 1;
        break;
      }

      case 'p': {
        const char *v = NULL;
        if (control_mode_selected) {
          fprintf(stderr, "control mode does not accept -p\n");
          return PARSE_ERR;
        }
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

  if (output_seen && client_action != 'g') {
    fprintf(stderr, "-o requires -g\n");
    return PARSE_ERR;
  }

  if (!server_selected && client_actions == 0 && !control_mode_selected) {
    fprintf(stderr, "must specify one of -d, -c, -g, -m, status, or stop\n");
    return PARSE_ERR;
  }

  if (server_selected && client_actions > 0) {
    fprintf(stderr, "cannot use server and client modes together\n");
    return PARSE_ERR;
  }

  if (client_actions > 1) {
    fprintf(stderr, "must choose exactly one client action: -c, -g, or -m\n");
    return PARSE_ERR;
  }

  if (!control_mode_selected) {
    opt->mode = server_selected ? server_mode : client_mode;
  }
  if (opt->mode == server_mode && opt->path == NULL) return PARSE_ERR;
  if (opt->mode == client_mode && client_action == 'c' && opt->path == NULL) return PARSE_ERR;
  if (opt->mode == client_mode && client_action == 'g' && opt->remote_path == NULL) return PARSE_ERR;
  if (opt->mode == client_mode && client_action == 'm' && opt->message == NULL) {
    return PARSE_ERR;
  }
  if (ip_seen && opt->mode != client_mode) {
    fprintf(stderr, "%s mode does not accept -i\n",
            opt->mode == server_mode ? "server" : "control");
    return PARSE_ERR;
  }

  return PARSE_OK;
}
