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
  const char *http_bind;
  uint16_t port;
  uint16_t http_port;
  uint8_t msg_type;
  uint8_t msg_flags;
} Opt;

typedef struct {
  const char *path; 
  const char *message;
  const char *ip;
  uint16_t port;
  uint8_t msg_type;
  uint8_t msg_flags;
} client_opt_t;


typedef struct {
  const char *path;
  const char *http_bind;
  uint16_t port;
  uint16_t http_port;
} server_opt_t;


void usage(const char *argv0);
int parse_port(const char *s, uint16_t *out);
int need_value(int argc, char **argv, int *i, const char **out);
parse_result_t parse_args(int argc, char **argv, Opt *opt);

#endif  // HF_CLI_H
