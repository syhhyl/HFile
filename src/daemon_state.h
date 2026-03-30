#ifndef HF_DAEMON_STATE_H
#define HF_DAEMON_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  long pid;
  char receive_dir[4096];
  char web_url[256];
  char log_path[4096];
  uint16_t port;
  int daemon_mode;
} daemon_state_t;

int daemon_state_default_pid_path(char *out, size_t out_cap);
int daemon_state_default_log_path(char *out, size_t out_cap);
int daemon_state_default_state_path(char *out, size_t out_cap);
int daemon_state_write(const daemon_state_t *state);
int daemon_state_read(daemon_state_t *state);
void daemon_state_cleanup_files(void);

#endif  // HF_DAEMON_STATE_H
