#ifndef HELPER_H
#define HELPER_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef DEBUG
#define DBG(fmt, ...) \
  fprintf(stderr, "[DBG %s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif




int get_file_name(const char **file_path, const char **file_name);

#endif
