/**
 * @file helper.c
 * @brief Common utility implementations for HFile
 */

#include "helper.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <winsock2.h>
#endif

/**
 * @brief Write all bytes to a file descriptor
 * @param fd   File descriptor
 * @param buf  Buffer containing data
 * @param len  Number of bytes to write
 * @return Number of bytes written on success, -1 on error
 *
 * Handles partial writes and EINTR automatically.
 */
ssize_t write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  size_t left = len;

  while (left > 0) {
#ifdef _WIN32
    ssize_t n = send(fd, (const char *)p, (int)left, 0);
#else
    ssize_t n = write(fd, p, left);
#endif
    if (n > 0) {
      p += (size_t)n;
      left -= (size_t)n;
      continue;
    }
    if (n == 0) {
      return -1;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;
  }

  return (ssize_t)len;
}

/**
 * @brief Read all bytes from a file descriptor
 * @param fd   File descriptor
 * @param buf  Buffer to store data
 * @param len  Number of bytes to read
 * @return Number of bytes read on success, -1 on error
 *
 * Handles partial reads and EINTR automatically.
 */
ssize_t read_all(int fd, void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  size_t left = len;

  while (left > 0) {
#ifdef _WIN32
    ssize_t n = recv(fd, (char *)p, (int)left, 0);
#else
    ssize_t n = read(fd, p, left);
#endif
    if (n > 0) {
      p += (size_t)n;
      left -= (size_t)n;
      continue;
    }
    if (n == 0) {
      errno = ECONNRESET;
      return -1;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;
  }

  return (ssize_t)len;
}

/**
 * @brief Print usage information
 * @param argv0 Program name
 */
void usage(const char *argv0) {
  fprintf(stderr,
          "usage:\n"
          "  %s -s <server_path> [-p <port>]\n"
          "  %s -c <file_path> [-i <ip>] [-p <port>]\n",
          argv0, argv0);
}

/**
 * @brief Parse port string to uint16_t
 * @param s    Port string
 * @param out  Output port number
 * @return 0 on success, 1 on error
 */
int parse_port(const char *s, uint16_t *out) {
  if (s == NULL || *s == '\0') {
    return 1;
  }
  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') {
    return 1;
  }
  if (v == 0 || v > 65535UL) {
    return 1;
  }
  *out = (uint16_t)v;
  return 0;
}

/**
 * @brief Check if next argument exists and return it
 * @param argc Argument count
 * @param argv Argument vector
 * @param i    Current argument index (in/out)
 * @param out  Output pointer to value
 * @return 0 on success, 1 if no value available
 */
int need_value(int argc, char **argv, int *i, const char **out) {
  if (*i + 1 >= argc) {
    return 1;
  }
  *i = *i + 1;
  *out = argv[*i];
  return 0;
}

/**
 * @brief Parse command-line arguments
 * @param argc Argument count
 * @param argv Argument vector
 * @param opt  Output options structure
 * @return Parse result (PARSE_OK, PARSE_HELP, PARSE_ERR)
 */
parse_result_t parse_args(int argc, char **argv, Opt *opt) {
  if (opt == NULL) {
    return PARSE_ERR;
  }

  opt->mode = init_mode;
  opt->path = NULL;
  opt->ip = "127.0.0.1";
  opt->port = 9000;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a == NULL || a[0] != '-' || a[1] == '\0' || a[2] != '\0') {
      fprintf(stderr, "invalid argument\n");
      return PARSE_ERR;
    }

    switch (a[1]) {
      case 'h':
        return PARSE_HELP;

      case 's': {
        if (opt->mode == client_mode) {
          fprintf(stderr, "cannot use -s -c together\n");
          return PARSE_ERR;
        }

        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid server path\n");
          return PARSE_ERR;
        }
        if (v[0] == '-') {
          fprintf(stderr, "invalid server path\n");
          return PARSE_ERR;
        }

        opt->mode = server_mode;
        opt->path = v;
        break;
      }

      case 'c': {
        if (opt->mode == server_mode) {
          fprintf(stderr, "cannot use -s -c together\n");
          return PARSE_ERR;
        }

        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid client path\n");
          return PARSE_ERR;
        }
        if (v[0] == '-') {
          fprintf(stderr, "invalid client path\n");
          return PARSE_ERR;
        }

        opt->mode = client_mode;
        opt->path = v;
        break;
      }

      case 'i': {
        if (opt->mode != client_mode) {
          fprintf(stderr, "server mode don't need ip\n");
          return PARSE_ERR;
        }

        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid argument\n");
          return PARSE_ERR;
        }
        if (v[0] == '-') {
          fprintf(stderr, "invalid argument\n");
          return PARSE_ERR;
        }

        opt->ip = v;
        break;
      }

      case 'p': {
        const char *v = NULL;
        if (need_value(argc, argv, &i, &v) != 0) {
          fprintf(stderr, "invalid port\n");
          return PARSE_ERR;
        }
        if (parse_port(v, &opt->port) != 0) {
          fprintf(stderr, "invalid port\n");
          return PARSE_ERR;
        }
        break;
      }

      default:
        return PARSE_ERR;
    }
  }

  if (opt->mode == init_mode) {
    return PARSE_ERR;
  }
  if (opt->path == NULL) {
    return PARSE_ERR;
  }

  return PARSE_OK;
}

#ifdef _WIN32
/**
 * @brief Initialize Windows Sockets (Winsock)
 * @return 0 on success, non-zero on error
 */
int winsock_init(void) {
  WSADATA wsa_data;
  return WSAStartup(MAKEWORD(2, 2), &wsa_data);
}

/**
 * @brief Cleanup Windows Sockets
 */
void winsock_cleanup(void) {
  WSACleanup();
}
#endif
