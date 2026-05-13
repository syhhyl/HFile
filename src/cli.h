#ifndef HF_CLI_H
#define HF_CLI_H

#include <stdint.h>

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
} Opt;


void usage(const char *argv0);
parse_result_t parse_args(int argc, char **argv, Opt *opt);

#endif  // HF_CLI_H
