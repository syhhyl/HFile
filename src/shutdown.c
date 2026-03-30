#include "shutdown.h"

#include <stddef.h>
#include <signal.h>

#ifdef _WIN32
  #include <windows.h>
#endif

static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_shutdown_signal = 0;

static void shutdown_set_requested(int sig) {
  g_shutdown_requested = 1;
  if (sig != 0) {
    g_shutdown_signal = sig;
  }
}

#ifdef _WIN32
static BOOL WINAPI shutdown_console_handler(DWORD ctrl_type) {
  switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      shutdown_set_requested(SIGINT);
      return TRUE;
    default:
      return FALSE;
  }
}
#else
static void shutdown_signal_handler(int sig) {
  shutdown_set_requested(sig);
}
#endif

int shutdown_init(void) {
  g_shutdown_requested = 0;
  g_shutdown_signal = 0;

#ifdef _WIN32
  if (SetConsoleCtrlHandler(shutdown_console_handler, TRUE) == 0) {
    return 1;
  }
#else
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = shutdown_signal_handler;
  if (sigaction(SIGINT, &sa, NULL) != 0) {
    return 1;
  }
  if (sigaction(SIGTERM, &sa, NULL) != 0) {
    return 1;
  }
#endif

  return 0;
}

void shutdown_cleanup(void) {
#ifdef _WIN32
  (void)SetConsoleCtrlHandler(shutdown_console_handler, FALSE);
#else
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = SIG_DFL;
  (void)sigaction(SIGINT, &sa, NULL);
  (void)sigaction(SIGTERM, &sa, NULL);
#endif
}

void shutdown_request(void) {
  shutdown_set_requested(0);
}

int shutdown_requested(void) {
  return g_shutdown_requested ? 1 : 0;
}

int shutdown_signal_number(void) {
  return (int)g_shutdown_signal;
}

int shutdown_exit_code(void) {
  return 130;
}
