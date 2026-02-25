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
  const char *ip;
  uint16_t port;
} Opt;

void usage(const char *argv0);
int parse_port(const char *s, uint16_t *out);
int need_value(int argc, char **argv, int *i, const char **out);
parse_result_t parse_args(int argc, char **argv, Opt *opt);