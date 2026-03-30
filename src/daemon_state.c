#include "daemon_state.h"

#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <process.h>
  #include <windows.h>
#else
  #include <unistd.h>
#endif

static long daemon_state_process_id(void) {
#ifdef _WIN32
  return (long)_getpid();
#else
  return (long)getpid();
#endif
}

static int daemon_state_copy_str(char *out, size_t out_cap, const char *value) {
  int n = 0;

  if (out == NULL || out_cap == 0 || value == NULL) {
    return 1;
  }

  n = snprintf(out, out_cap, "%s", value);
  return (n < 0 || (size_t)n >= out_cap) ? 1 : 0;
}

static int daemon_state_write_text_file(const char *path, const char *text) {
  char tmp_path[4096];
  int fd = -1;

  if (path == NULL || text == NULL) {
    return 1;
  }

  if (fs_make_temp_path(tmp_path, sizeof(tmp_path), path,
                        (int)daemon_state_process_id(), 0) != 0) {
    return 1;
  }

  fd = fs_open_temp_file(tmp_path);
  if (fd == -1) {
    return 1;
  }

  size_t len = strlen(text);
  if (fs_write_all(fd, text, len) != (ssize_t)len) {
    (void)fs_close(fd);
    fs_remove_quiet(tmp_path);
    return 1;
  }
  if (fs_close(fd) != 0) {
    fs_remove_quiet(tmp_path);
    return 1;
  }
  if (fs_finalize_temp_file(tmp_path, path, NULL) != 0) {
    fs_remove_quiet(tmp_path);
    return 1;
  }

  return 0;
}

static int daemon_state_tmp_dir(char *out, size_t out_cap) {
#ifdef _WIN32
  DWORD len = GetTempPathA((DWORD)out_cap, out);
  if (len == 0 || len >= (DWORD)out_cap) {
    return 1;
  }
  return 0;
#else
  const char *tmp = getenv("TMPDIR");
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = "/tmp";
  }
  return daemon_state_copy_str(out, out_cap, tmp);
#endif
}

static int daemon_state_global_path(const char *name, char *out, size_t out_cap) {
  char tmp_dir[4096];

  if (daemon_state_tmp_dir(tmp_dir, sizeof(tmp_dir)) != 0) {
    return 1;
  }
  return fs_join_path(out, out_cap, tmp_dir, name);
}

static int daemon_state_parse_u16(const char *value, uint16_t *out) {
  char *end = NULL;
  unsigned long n = 0;

  if (value == NULL || out == NULL || value[0] == '\0') {
    return 1;
  }

  errno = 0;
  n = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || n > 65535UL) {
    return 1;
  }
  *out = (uint16_t)n;
  return 0;
}

static int daemon_state_parse_long(const char *value, long *out) {
  char *end = NULL;
  long n = 0;

  if (value == NULL || out == NULL || value[0] == '\0') {
    return 1;
  }

  errno = 0;
  n = strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') {
    return 1;
  }
  *out = n;
  return 0;
}

int daemon_state_default_pid_path(char *out, size_t out_cap) {
  return daemon_state_global_path("hf-daemon.pid", out, out_cap);
}

int daemon_state_default_log_path(char *out, size_t out_cap) {
  return daemon_state_global_path("hf-daemon.log", out, out_cap);
}

int daemon_state_default_state_path(char *out, size_t out_cap) {
  return daemon_state_global_path("hf-daemon.state", out, out_cap);
}

int daemon_state_write(const daemon_state_t *state) {
  char state_path[4096];
  char text[12288];
  int n = 0;

  if (state == NULL || daemon_state_default_state_path(state_path, sizeof(state_path)) != 0) {
    return 1;
  }

  n = snprintf(text, sizeof(text),
               "pid=%ld\n"
               "receive_dir=%s\n"
               "port=%u\n"
               "web_url=%s\n"
               "log_path=%s\n"
               "daemon_mode=%d\n",
               state->pid,
               state->receive_dir,
               (unsigned)state->port,
               state->web_url,
               state->log_path,
               state->daemon_mode);
  if (n < 0 || (size_t)n >= sizeof(text)) {
    return 1;
  }

  return daemon_state_write_text_file(state_path, text);
}

int daemon_state_read(daemon_state_t *state) {
  FILE *fp = NULL;
  char state_path[4096];
  char line[4608];

  if (state == NULL || daemon_state_default_state_path(state_path, sizeof(state_path)) != 0) {
    return 1;
  }

  fp = fopen(state_path, "r");
  if (fp == NULL) {
    return 1;
  }

  memset(state, 0, sizeof(*state));
  while (fgets(line, sizeof(line), fp) != NULL) {
    char *eq = strchr(line, '=');
    char *value = NULL;
    if (eq == NULL) {
      continue;
    }

    *eq = '\0';
    value = eq + 1;
    value[strcspn(value, "\r\n")] = '\0';

    if (strcmp(line, "pid") == 0) {
      (void)daemon_state_parse_long(value, &state->pid);
    } else if (strcmp(line, "receive_dir") == 0) {
      (void)daemon_state_copy_str(state->receive_dir, sizeof(state->receive_dir), value);
    } else if (strcmp(line, "port") == 0) {
      (void)daemon_state_parse_u16(value, &state->port);
    } else if (strcmp(line, "web_url") == 0) {
      (void)daemon_state_copy_str(state->web_url, sizeof(state->web_url), value);
    } else if (strcmp(line, "log_path") == 0) {
      (void)daemon_state_copy_str(state->log_path, sizeof(state->log_path), value);
    } else if (strcmp(line, "daemon_mode") == 0) {
      long daemon_mode = 0;
      if (daemon_state_parse_long(value, &daemon_mode) == 0) {
        state->daemon_mode = daemon_mode != 0 ? 1 : 0;
      }
    }
  }

  (void)fclose(fp);
  return state->pid > 0 ? 0 : 1;
}

void daemon_state_cleanup_files(void) {
  char pid_path[4096];
  char state_path[4096];

  if (daemon_state_default_pid_path(pid_path, sizeof(pid_path)) == 0) {
    fs_remove_quiet(pid_path);
  }
  if (daemon_state_default_state_path(state_path, sizeof(state_path)) == 0) {
    fs_remove_quiet(state_path);
  }
}
