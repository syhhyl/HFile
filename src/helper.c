#include "helper.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif


int get_file_name(const char **file_path, const char **file_name) {
  if (*file_path == NULL) return 1;
  char *tmp;
#ifdef _WIN32
  tmp = strrchr(*file_path, '\\');
#else
  tmp = strrchr(*file_path, '/');
#endif
  if (tmp == NULL) *file_name = *file_path;
  else *file_name = tmp + 1;
  return *file_name ? 0 : 1;
}

uint64_t now_ns() {
#ifdef _WIN32
  static LARGE_INTEGER freq = {0};
  LARGE_INTEGER t;
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t);
  return (uint64_t)(((long double)t.QuadPart * 1000000000.0L) /
                    (long double)freq.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

double ns_to_s(uint64_t ns) {
  return (double)ns * 1e-9;
}

void report_transfer_perf(
    const char *mode, int ok, double total_s,
    double io_s, double net_s, uint64_t file_bytes,
    uint64_t wire_bytes) {

  fprintf(stderr,
    "perf mode=%s ok=%d total=%.4fs io=%.4fs net=%.4fs "
    "file_bytes=%" PRIu64 "B wire_bytes=%" PRIu64 "B\n", 
    mode, ok, total_s, io_s, net_s, file_bytes, wire_bytes);
}
