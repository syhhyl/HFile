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

int main(int argc, char **argv) {
  Mode mode = MODE_RECV;
  const char *path = NULL;
  const char *ip = NULL;
  uint16_t port = 8888;
  int show_usage = 0;
  int parse_error = 0;
  int ip_seen = 0;
  int positional_seen = 0;

  if (argc < 2 || argv[1] == NULL) {
    fprintf(stderr, "missing command\n");
    parse_error = 1;
    goto usage;
  }

  if (strcmp(argv[1], "-h") == 0) {
    show_usage = 1;
    goto usage;
  }

  if (strcmp(argv[1], "recv") == 0) {
    mode = MODE_RECV;
  } else if (strcmp(argv[1], "send") == 0) {
    mode = MODE_SEND;
  } else {
    fprintf(stderr, "unknown command\n");
    parse_error = 1;
    goto usage;
  }

  for (int i = 2; i < argc; i++) {
    const char *a = argv[i];
    if (a == NULL) {
      fprintf(stderr, "invalid argument\n");
      parse_error = 1;
      goto usage;
    }

    if (a[0] != '-' || a[1] == '\0') {
      if (positional_seen) {
        fprintf(stderr, "unexpected extra argument\n");
        parse_error = 1;
        goto usage;
      }
      path = a;
      positional_seen = 1;
      continue;
    }

    if (a[1] == '-') {
      fprintf(stderr, "invalid argument\n");
      parse_error = 1;
      goto usage;
    }

    if (a[2] != '\0') {
      fprintf(stderr, "invalid argument\n");
      parse_error = 1;
      goto usage;
    }

    switch (a[1]) {
      case 'h':
        show_usage = 1;
        goto usage;

      case 'i': {
        if (mode == MODE_RECV) {
          fprintf(stderr, "recv mode does not accept -i\n");
          parse_error = 1;
          goto usage;
        }
        if (i + 1 >= argc) {
          fprintf(stderr, "invalid address\n");
          parse_error = 1;
          goto usage;
        }
        ip = argv[++i];
        ip_seen = 1;
        break;
      }

      case 'p': {
        if (i + 1 >= argc) {
          fprintf(stderr, "invalid port\n");
          parse_error = 1;
          goto usage;
        }
        const char *s = argv[++i];
        if (s == NULL || *s == '\0') {
          fprintf(stderr, "invalid port\n");
          parse_error = 1;
          goto usage;
        }
        errno = 0;
        char *end = NULL;
        unsigned long v = strtoul(s, &end, 10);
        if (errno != 0 || end == s || *end != '\0' ||
            v == 0 || v > 65535UL) {
          fprintf(stderr, "invalid port\n");
          parse_error = 1;
          goto usage;
        }
        port = (uint16_t)v;
        break;
      }

      default:
        fprintf(stderr, "invalid argument\n");
        parse_error = 1;
        goto usage;
    }
  }

  if (mode == MODE_RECV && path == NULL) {
    path = ".";
  }

  if (mode == MODE_SEND && path == NULL) {
    fprintf(stderr, "missing file to send\n");
    parse_error = 1;
    goto usage;
  }

  if (ip_seen && mode != MODE_SEND) {
    fprintf(stderr, "recv mode does not accept -i\n");
    parse_error = 1;
    goto usage;
  }

  return mode == MODE_RECV ? node_recv(path, port) : node_send(path, ip, port);

usage:
  if (show_usage || parse_error) {
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
      argv[0], argv[0], argv[0], argv[0], argv[0]);
  }
  return show_usage ? 0 : 1;
}
