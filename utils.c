#include <string.h>
#include <stdlib.h>
#include "utils.h"
#include <stdio.h>
#include <unistd.h>

int get_file_name(char *file_path, char **file_name) {
  char *end = strrchr(file_path, '/');
  if (end == NULL) {
    *file_name = strdup(file_path);
    return 0;
  }
  size_t file_name_len = strlen(end + 1);
  *file_name = (char *)malloc(file_name_len + 1);
  if (*file_name == NULL) {
    perror("memory allocation failed\n");
    return 1;
  }
  
  strcpy(*file_name, end+1);
  return 0;
}

int read_full(int fd, void *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = read(fd, (char *)buf + off, n - off);
    if (r == 0) return 0;
    
  }
}