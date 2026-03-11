#ifndef HF_CLI_H
#define HF_CLI_H

#include <stdint.h>

typedef enum {
  PARSE_OK,
  PARSE_HELP,
  PARSE_ERR
} parse_result_t;

typedef enum {
  server_mode,
  client_mode,
  init_mode
} Mode;

typedef struct {
  Mode mode;
  const char *path;
  const char *message;
  const char *ip;
  uint16_t port;
  int perf;
  int compress;
  uint8_t msg_type;
  uint8_t msg_flags;
} Opt;

typedef struct {
  const char *path; 
  const char *message;
  const char *ip;
  uint16_t port;
  int perf;
  uint8_t msg_type;
  uint8_t msg_flags;
} client_opt_t;


typedef struct {
  const char *path;
  uint16_t port;
  int perf;
} server_opt_t;


void usage(const char *argv0);
int parse_port(const char *s, uint16_t *out);
int need_value(int argc, char **argv, int *i, const char **out);
parse_result_t parse_args(int argc, char **argv, Opt *opt);

#endif  // HF_CLI_H
