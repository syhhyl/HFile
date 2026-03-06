#ifndef HELPER_H
#define HELPER_H

#include <stdint.h>


#ifdef DEBUG
#define DBG(fmt, ...) \
  fprintf(stderr, "[DBG %s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif


#define CHUNK_SIZE 1024 * 1024

int get_file_name(const char **file_path, const char **file_name);
void report_transfer_perf(const char *mode, int ok, double total_s,
                          double io_s, double net_s, uint64_t file_bytes,
                          uint64_t wire_bytes);

uint64_t now_ns();
double ns_to_s(uint64_t ns);

#endif //HELPER_H
