#include "control.h"

#include "daemon_state.h"
#include "net.h"
#include "qrcodegen.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <io.h>
  #include <windows.h>
#else
  #include <signal.h>
  #include <unistd.h>
#endif

static int control_is_tty_stdout(void) {
#ifdef _WIN32
  return _isatty(_fileno(stdout)) ? 1 : 0;
#else
  return isatty(fileno(stdout)) ? 1 : 0;
#endif
}

static int control_use_ansi_stdout(void) {
#ifdef _WIN32
  return 0;
#else
  return control_is_tty_stdout();
#endif
}

static const char *control_ansi_reset(void) {
  return "\033[0m";
}

static const char *control_ansi_bold(void) {
  return "\033[1m";
}

static const char *control_ansi_dim(void) {
  return "\033[2m";
}

static const char *control_ansi_cyan(void) {
  return "\033[36m";
}

static const char *control_ansi_green(void) {
  return "\033[32m";
}

static const char *control_ansi_yellow(void) {
  return "\033[33m";
}

static void control_print_rich_divider(FILE *out) {
  if (out == NULL) {
    return;
  }

  if (control_use_ansi_stdout()) {
    fprintf(out, "%s----------------------------------------%s\n",
            control_ansi_dim(), control_ansi_reset());
  } else {
    fprintf(out, "----------------------------------------\n");
  }
}

static void control_print_rich_title(FILE *out,
                                     const char *title,
                                     const char *state,
                                     const char *state_color) {
  if (out == NULL || title == NULL || state == NULL) {
    return;
  }

  if (control_use_ansi_stdout()) {
    fprintf(out, "%s%s%s  %s%s%s\n",
            control_ansi_bold(), title, control_ansi_reset(),
            state_color != NULL ? state_color : "",
            state,
            control_ansi_reset());
  } else {
    fprintf(out, "%s  %s\n", title, state);
  }
}

static void control_print_rich_field(FILE *out,
                                     const char *label,
                                     const char *value) {
  if (out == NULL || label == NULL || value == NULL) {
    return;
  }

  if (control_use_ansi_stdout()) {
    fprintf(out, "  %s%-12s%s %s\n",
            control_ansi_cyan(), label, control_ansi_reset(), value);
  } else {
    fprintf(out, "  %-12s %s\n", label, value);
  }
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

int control_build_url(uint16_t port,
                      char *url_out,
                      size_t url_out_cap,
                      int *phone_reachable_out) {
  const char *host = NULL;
  char discovered_ip[64];
  if (url_out == NULL || url_out_cap == 0 || phone_reachable_out == NULL ||
      port == 0) {
    return 1;
  }

  if (net_primary_ipv4(discovered_ip, sizeof(discovered_ip)) == 0) {
    host = discovered_ip;
    *phone_reachable_out = 1;
  } else {
    host = "127.0.0.1";
    *phone_reachable_out = 0;
  }

  {
    int n = snprintf(url_out, url_out_cap, "http://%s:%u/", host,
                     (unsigned)port);
    if (n < 0 || (size_t)n >= url_out_cap) {
      return 1;
    }
  }

  return 0;
}

static int control_print_qr_to(FILE *out, const char *url) {
  uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
  uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
  const int border = 4;

  if (out == NULL || url == NULL || url[0] == '\0') {
    return 1;
  }

  if (!qrcodegen_encodeText(url, temp, qr, qrcodegen_Ecc_MEDIUM,
                            qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                            qrcodegen_Mask_AUTO, true)) {
    return 1;
  }

  {
    int size = qrcodegen_getSize(qr);
    for (int y = -border; y < size + border; y += 2) {
      for (int x = -border; x < size + border; x++) {
        int top = (x >= 0 && x < size && y >= 0 && y < size)
                    ? qrcodegen_getModule(qr, x, y)
                    : 0;
        int bottom = (x >= 0 && x < size && (y + 1) >= 0 && (y + 1) < size)
                       ? qrcodegen_getModule(qr, x, y + 1)
                       : 0;
        if (top && bottom) {
          fputs("\xE2\x96\x88", out);
        } else if (top) {
          fputs("\xE2\x96\x80", out);
        } else if (bottom) {
          fputs("\xE2\x96\x84", out);
        } else {
          fputc(' ', out);
        }
      }
      fputc('\n', out);
    }
  }

  if (ferror(out)) {
    return 1;
  }

  return 0;
}

void control_print_server_access_details(FILE *out,
                                         const char *receive_dir,
                                         uint16_t port,
                                         const char *log_path,
                                         long pid,
                                         int daemon_mode) {
  char url[256];
  const char *listen_target = NULL;
  int phone_reachable = 0;

  if (out == NULL || receive_dir == NULL) {
    return;
  }

  (void)log_path;
  url[0] = '\0';

  if (out == stdout && control_is_tty_stdout()) {
    char listen_buf[128];

    if (control_build_url(port, url, sizeof(url), &phone_reachable) == 0) {
      listen_target = strstr(url, "://");
      listen_target = listen_target != NULL ? listen_target + 3 : url;
      {
        int n = snprintf(listen_buf, sizeof(listen_buf), "%.*s (tcp + http)",
                         (int)strcspn(listen_target, "/"), listen_target);
        if (n < 0 || (size_t)n >= sizeof(listen_buf)) {
          listen_buf[0] = '\0';
        }
      }
    } else {
      (void)snprintf(listen_buf, sizeof(listen_buf), "127.0.0.1:%u (tcp + http)",
                     (unsigned)port);
    }

    control_print_rich_divider(out);
    control_print_rich_title(out, "HFile",
                             daemon_mode ? "DAEMON READY" : "SERVER READY",
                             control_ansi_green());
    control_print_rich_field(out, "Receive Dir", receive_dir);
    control_print_rich_field_num(out, "Port", (unsigned long)port);
    if (daemon_mode) {
      control_print_rich_field_num(out, "PID", (unsigned long)pid);
    }
    if (listen_buf[0] != '\0') {
      control_print_rich_field(out, "Listen", listen_buf);
    }
    if (url[0] != '\0') {
      control_print_rich_field(out, "Web UI", url);
      if (!phone_reachable) {
        control_print_rich_field(out, "Mobile",
                                 "LAN IPv4 not detected, using localhost");
      }
    }
    if (!daemon_mode) {
      control_print_rich_field(out, "Status", "Waiting for files and messages");
    }
    control_print_rich_divider(out);

    if (url[0] != '\0') {
      (void)control_print_qr_to(out, url);
    }
    fflush(out);
    return;
  }

  fprintf(out, daemon_mode ? "HFile daemon ready\n" : "HFile server ready\n");
  fprintf(out, "  receive dir: %s\n", receive_dir);
  if (daemon_mode) {
    fprintf(out, "  pid        : %ld\n", pid);
  }

  if (control_build_url(port, url, sizeof(url), &phone_reachable) == 0) {
    listen_target = strstr(url, "://");
    listen_target = listen_target != NULL ? listen_target + 3 : url;
    fprintf(out, "  listen     : %.*s (tcp + http)\n",
            (int)strcspn(listen_target, "/"), listen_target);
    fprintf(out, "  web ui     : %s\n", url);
    if (!phone_reachable) {
      fprintf(out, "  mobile     : could not detect a LAN IPv4, using localhost\n");
    }

    if (!daemon_mode) {
      fprintf(out, "  status     : waiting for files and messages\n");
    }
    if (out == stdout && control_is_tty_stdout()) {
      (void)control_print_qr_to(out, url);
    }
  } else {
    fprintf(out, "  listen     : 127.0.0.1:%u (tcp + http)\n", (unsigned)port);
  }

  fflush(out);
}

static void control_print_status(FILE *out, const daemon_state_t *state) {
  if (out == NULL || state == NULL) {
    return;
  }

  if (out == stdout && control_is_tty_stdout()) {
    control_print_rich_divider(out);
    control_print_rich_title(out, "HFile", "RUNNING", control_ansi_green());
    control_print_rich_field_num(out, "PID", (unsigned long)state->pid);
    control_print_rich_field(out, "Receive Dir", state->receive_dir);
    control_print_rich_field_num(out, "Port", (unsigned long)state->port);
    control_print_rich_field(out, "Web UI", state->web_url);
    control_print_rich_divider(out);
    if (state->web_url[0] != '\0') {
      (void)control_print_qr_to(out, state->web_url);
    }
    return;
  }

  fprintf(out, "status: running\n");
  fprintf(out, "pid: %ld\n", state->pid);
  fprintf(out, "receive dir: %s\n", state->receive_dir);
  fprintf(out, "port: %u\n", (unsigned)state->port);
  fprintf(out, "web ui: %s\n", state->web_url);
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

int control_status(void) {
  daemon_state_t state;

  if (control_load_running_state(&state) != 0) {
    if (control_is_tty_stdout()) {
      control_print_rich_divider(stdout);
      control_print_rich_title(stdout, "HFile", "STOPPED", control_ansi_yellow());
      control_print_rich_field(stdout, "Status", "No running daemon");
      control_print_rich_divider(stdout);
      return 1;
    }
    printf("status: stopped\n");
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
  if (control_is_tty_stdout()) {
    control_print_rich_divider(stdout);
    control_print_rich_title(stdout, "HFile", "STOPPED", control_ansi_yellow());
    control_print_rich_field_num(stdout, "PID", (unsigned long)state.pid);
    control_print_rich_field(stdout, "Result", "Server stopped");
    control_print_rich_divider(stdout);
    return 0;
  }
  printf("stopped pid %ld\n", state.pid);
  return 0;
}
