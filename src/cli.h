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
} Mode;

typedef struct {
  Mode mode;
  const char *path;
  const char *ip;
  uint16_t port;
} Opt;


void usage(const char *argv0);
parse_result_t parse_args(int argc, char **argv, Opt *opt);
int load_windows_utf8_argv(int *argc_out, char ***argv_out);
void free_windows_argv(char **argv, int argc);

#endif  // HF_CLI_H
