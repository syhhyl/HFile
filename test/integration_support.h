#ifndef HF_INTEGRATION_SUPPORT_H
#define HF_INTEGRATION_SUPPORT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define OUT_CAP 65536

extern char hf_path[4096];
extern char g_project_root[4096];
extern char test_dir[256];

typedef struct {
  pid_t pid;
  uint16_t port;
  int output_fd;
} receiver_t;

#define RECEIVER_INIT { -1, 0, -1 }

int integration_init(int argc, char **argv);
void integration_cleanup(void);

int spawn_hf(char *const argv[], char *out_buf, char *err_buf);
int start_recv(const char *dir, receiver_t *receiver);
void stop_recv(receiver_t *receiver);
int tcp_connect(const char *host, uint16_t port);
int send_preamble(int sock, uint16_t magic, uint8_t ver,
                  uint8_t type, uint8_t flags, uint64_t payload,
                  const char *name, uint16_t nlen, uint64_t fsize);
int recv_response(int sock, uint8_t *phase, uint8_t *status);

int file_exists(const char *path);
long file_size(const char *path);
int write_file(const char *path, const void *data, size_t len);
int read_file(const char *path, char *buf, size_t cap);
int files_equal(const char *a, const char *b);
int rm_rf(const char *path);
void make_tmp_path(char *buf, size_t cap, const char *name);
int count_tmp_files(const char *dir);

#endif
