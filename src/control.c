#include "control.h"

#include "daemon_state.h"
#include "mobile_ui.h"

#include <errno.h>
#include <stdio.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <signal.h>
  #include <unistd.h>
#endif

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

int control_status(void) {
  daemon_state_t state;

  if (control_load_running_state(&state) != 0) {
    printf("status: stopped\n");
    return 1;
  }

  printf("status: running\n");
  printf("pid: %ld\n", state.pid);
  printf("receive dir: %s\n", state.receive_dir);
  printf("port: %u\n", (unsigned)state.port);
  printf("web ui: %s\n", state.web_url);
  printf("error log: %s\n", state.log_path);
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
  printf("stopped pid %ld\n", state.pid);
  return 0;
}

int control_print_qr(void) {
  daemon_state_t state;

  if (control_load_running_state(&state) != 0) {
    fprintf(stderr, "no running daemon found\n");
    return 1;
  }
  return mobile_ui_print_qr(stdout, state.web_url);
}
