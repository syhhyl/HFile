#include "helper.h"
#include "stdint.h"
#include "unistd.h"
#include "errno.h"



ssize_t write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  size_t left = len;
  
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n > 0) {
      p += (size_t)n;
      left -= (size_t)n;
      continue;
    }
    if (n == 0) return -1;
    if (errno == EINTR) continue;
    
    return -1;
  }
  
  return (ssize_t)len;
}

ssize_t read_all(int fd, void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  size_t left = len;
  
  while (left > 0) {
    ssize_t n = read(fd, p, left);
    if (n > 0) {
      p += (size_t)n;
      left -= (size_t)n;
      continue;
    }
    if (n == 0) {
      errno = ECONNRESET;
      return -1;
    }
    if (errno == EINTR) continue;
    return -1;
  }

  return (ssize_t)len;
}

void usage(const char *argv0) {
  fprintf(stderr,
          "usage:\n"
          "  %s -s <server_path> [-p <port>]\n"
          "  %s -c <file_path> [-i <ip>] [-p <port>]\n",
          argv0, argv0);
}


int parse_port(const char *s, uint16_t *out) {
  if (s == NULL || *s == '\0') return 1;
  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return 1;
  if (v == 0 || v > 65535UL) return 1;
  *out = (uint16_t)v;
  return 0;
}