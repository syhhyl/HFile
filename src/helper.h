// debug.h
#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#ifdef DEBUG
  #define DBG(fmt, ...) \
    fprintf(stderr, "[DBG %s:%d] " fmt "\n", \
            __FILE__, __LINE__, ##__VA_ARGS__)
#else
  #define DBG(...) ((void)0)
#endif

#endif // DEBUG_H

typedef enum {
  server_mode,
  client_mode,
  init_mode
} Mode;

typedef struct {
  Mode mode;
  const char *path;
  const char *ip;
  uint16_t port;
  int exit_code;
  bool help;
} Opt;

ssize_t write_all(int fd, const void *buf, size_t len);
ssize_t read_all(int fd, void *buf, size_t len);

void usage(const char *argv0);
int parse_port(const char *s, uint16_t *out);
int need_value(int argc, char **argv, int *i, const char **out);
int parse_args(int argc, char **argv, Opt *opt);






