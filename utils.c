#include <string.h>
#include <stdlib.h>
#include "utils.h"
#include <stdio.h>

void get_file_name(char *file_path, char **file_name) {
  char *end = strrchr(file_path, '/');
  if (end == NULL) {
    *file_name = strdup(file_path);
    return;
  }
  size_t file_name_len = strlen(end + 1);
  *file_name = (char *)malloc(file_name_len + 1);
  if (*file_name == NULL) {
    perror("memory allocation failed\n");
    exit(1);
  }
  
  strcpy(*file_name, end+1);
}