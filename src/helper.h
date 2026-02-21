#ifndef HELPER_H
#define HELPER_H

#include <stdint.h>
#include <stdio.h>

#ifdef DEBUG
#define DBG(fmt, ...) \
  fprintf(stderr, "[DBG %s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

#define CHUNK_SIZE 1024 * 1024

typedef enum {
  server_mode,
  client_mode,
  init_mode
} Mode;

typedef enum {
  PARSE_OK,
  PARSE_HELP,
  PARSE_ERR
} parse_result_t;

typedef struct {
  Mode mode;
  const char *path;
  const char *ip;
  uint16_t port;
} Opt;


int client(const char *path, const char *ip, uint16_t port);
int server(const char *path, uint16_t port);


ssize_t write_all(int fd, const void *buf, size_t len);
ssize_t read_all(int fd, void *buf, size_t len);

void usage(const char *argv0);
int parse_port(const char *s, uint16_t *out);
int need_value(int argc, char **argv, int *i, const char **out);
parse_result_t parse_args(int argc, char **argv, Opt *opt);

#endif




