#ifndef HELPER_H
#define HELPER_H


#ifdef DEBUG
#define DBG(fmt, ...) \
  fprintf(stderr, "[DBG %s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif


#define CHUNK_SIZE 1024 * 1024

int get_file_name(const char **file_path, const char **file_name);

#endif
