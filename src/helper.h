/**
 * @file helper.h
 * @brief Common definitions and utilities for HFile
 */

#ifndef HELPER_H
#define HELPER_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Platform detection and compatibility layer */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>

  /* MSVC uses different APIs */
  #ifdef _MSC_VER
    #define ssize_t int
  #endif

  /* Socket close function: Windows uses closesocket(), Unix uses close() */
  #define close_socket closesocket
#else
  #include <sys/types.h>
  #include <unistd.h>
  #define close_socket close
#endif

/* Debug logging macro */
#ifdef DEBUG
  #define DBG(fmt, ...) \
    fprintf(stderr, "[DBG %s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
  #define DBG(...) ((void)0)
#endif

/* I/O buffer size: 1MB */
#define CHUNK_SIZE 1024 * 1024

/* Operation mode */
typedef enum {
  server_mode,
  client_mode,
  init_mode
} Mode;

/* Command-line argument parsing result */
typedef enum {
  PARSE_OK,
  PARSE_HELP,
  PARSE_ERR
} parse_result_t;

/* Command-line options */
typedef struct {
  Mode mode;
  const char *path;
  const char *ip;
  uint16_t port;
} Opt;

/* Network functions */
int client(const char *path, const char *ip, uint16_t port);
int server(const char *path, uint16_t port);

/* I/O helper functions */
ssize_t write_all(int fd, const void *buf, size_t len);
ssize_t read_all(int fd, void *buf, size_t len);

/* Command-line argument parsing */
void usage(const char *argv0);
int parse_port(const char *s, uint16_t *out);
int need_value(int argc, char **argv, int *i, const char **out);
parse_result_t parse_args(int argc, char **argv, Opt *opt);

/* Windows-specific initialization */
#ifdef _WIN32
int winsock_init(void);
void winsock_cleanup(void);
#endif

#endif /* HELPER_H */
