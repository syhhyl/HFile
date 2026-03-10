#ifndef HELPER_H
#define HELPER_H

#include <stdint.h>

void report_transfer_perf(const char *mode, int ok, double total_s,
                          double io_s, double net_s, uint64_t file_bytes,
                          uint64_t wire_bytes);

uint64_t now_ns();
double ns_to_s(uint64_t ns);

#endif //HELPER_H