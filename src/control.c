#include "control.h"

#include "daemon_state.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <signal.h>
  #include <unistd.h>
#endif

static void control_print_rich_field(FILE *out,
                                     const char *label,
                                     const char *value) {
  if (out == NULL || label == NULL || value == NULL) {
    return;
  }

  fprintf(out, "  %-12s %s\n", label, value);
}

static void control_print_rich_field_num(FILE *out,
                                         const char *label,
                                         unsigned long value) {
  char buf[64];
  int n;

  if (out == NULL || label == NULL) {
    return;
  }

  n = snprintf(buf, sizeof(buf), "%lu", value);
  if (n < 0 || (size_t)n >= sizeof(buf)) {
    return;
  }
  control_print_rich_field(out, label, buf);
}

static void control_print_startup_banner(FILE *out) {
  static const char *lines[] = {
    "    __ _______ __   ",
    "   / // / __(_) /__ ",
    "  / _  / _// / / -_)",
    " /_//_/_/ /_/_/\\__/",
    "https://github.com/syhhyl/HFile"
  };
  size_t i;

  if (out == NULL) {
    return;
  }

  for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
    fprintf(out, "%s\n", lines[i]);
  }
}

void control_print_server_access_details(FILE *out,
                                          const char *receive_dir,
                                          uint16_t port,
                                         const char *log_path,
                                         long pid,
                                         int daemon_mode) {
  if (out == NULL || receive_dir == NULL) {
    return;
  }

  (void)port;
  (void)log_path;
  (void)pid;

  control_print_startup_banner(out);
  control_print_rich_field(out, "Receive Dir", receive_dir);
  if (!daemon_mode) {
    control_print_rich_field(out, "Status", "Waiting for files and messages");
  }

  fflush(out);
}

static void control_print_status(FILE *out, const daemon_state_t *state) {
  if (out == NULL || state == NULL) {
    return;
  }

  control_print_startup_banner(out);
  control_print_rich_field(out, "Receive Dir", state->receive_dir);
  control_print_rich_field_num(out, "PID", (unsigned long)state->pid);
}

static int control_pid_is_running(long pid) {
  if (pid <= 0) {
    return 0;
  }

#ifdef _WIN32
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
  if (process == NULL) {
    return 0;
  }
  DWORD wait_res = WaitForSingleObject(process, 0);
  CloseHandle(process);
  return wait_res == WAIT_TIMEOUT ? 1 : 0;
#else
  if (kill((pid_t)pid, 0) == 0) {
    return 1;
  }
  return errno == EPERM ? 1 : 0;
#endif
}

static int control_load_running_state(daemon_state_t *state) {
  if (daemon_state_read(state) != 0 || !control_pid_is_running(state->pid)) {
    daemon_state_cleanup_files();
    return 1;
  }
  return 0;
}

static int control_report_already_stopped(void) {
  daemon_state_cleanup_files();
  printf("already stopped\n");
  return 0;
}

int control_status(void) {
  daemon_state_t state;

  if (control_load_running_state(&state) != 0) {
    control_print_startup_banner(stdout);
    control_print_rich_field(stdout, "Status", "No running daemon");
    return 1;
  }

  control_print_status(stdout, &state);
  return 0;
}

int control_stop(void) {
  daemon_state_t state;

  if (control_load_running_state(&state) != 0) {
    printf("already stopped\n");
    return 0;
  }

#ifdef _WIN32
  HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                (DWORD)state.pid);
  if (process == NULL) {
    if (GetLastError() == ERROR_INVALID_PARAMETER) {
      return control_report_already_stopped();
    }
    fprintf(stderr, "failed to open process %ld\n", state.pid);
    return 1;
  }
  if (!TerminateProcess(process, 0)) {
    CloseHandle(process);
    fprintf(stderr, "failed to stop process %ld\n", state.pid);
    return 1;
  }
  if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) {
    CloseHandle(process);
    fprintf(stderr, "timed out waiting for process %ld to stop\n", state.pid);
    return 1;
  }
  CloseHandle(process);
#else
  if (kill((pid_t)state.pid, SIGTERM) != 0) {
    if (errno == ESRCH) {
      return control_report_already_stopped();
    }
    perror("kill(SIGTERM)");
    return 1;
  }
  for (int i = 0; i < 100; i++) {
    if (!control_pid_is_running(state.pid)) {
      break;
    }
    usleep(50000);
  }
  if (control_pid_is_running(state.pid)) {
    fprintf(stderr, "timed out waiting for pid %ld to stop\n", state.pid);
    return 1;
  }
#endif

  daemon_state_cleanup_files();
  control_print_startup_banner(stdout);
  control_print_rich_field_num(stdout, "PID", (unsigned long)state.pid);
  control_print_rich_field(stdout, "Result", "Server stopped");
  return 0;
}
