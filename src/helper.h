#ifndef HELPER_H
#define HELPER_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>


#ifdef DEBUG
#define DBG(fmt, ...) \
  fprintf(stderr, "[DBG %s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif



int client(const char *path, const char *ip, uint16_t port);
int server(const char *path, uint16_t port);



ssize_t write_all(int fd, const void *buf, size_t len);
int get_file_name(const char **file_path, const char **file_name);

#endif
