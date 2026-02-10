// debug.h
#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

#ifdef DEBUG
  #define DBG(fmt, ...) \
    fprintf(stderr, "[DBG %s:%d] " fmt "\n", \
            __FILE__, __LINE__, ##__VA_ARGS__)
#else
  #define DBG(...) ((void)0)
#endif

#endif // DEBUG_H

ssize_t write_all(int fd, const void *buf, size_t len);
ssize_t read_all(int fd, void *buf, size_t len);