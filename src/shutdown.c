#include "shutdown.h"

#include <stdatomic.h>
#include <stddef.h>
#include <signal.h>

static _Atomic int g_shutdown_requested = 0;
static _Atomic int g_shutdown_signal = 0;

static void shutdown_set_requested(int sig) {
  atomic_store(&g_shutdown_requested, 1);
  if (sig != 0) {
    atomic_store(&g_shutdown_signal, sig);
  }
}

static void shutdown_signal_handler(int sig) {
  shutdown_set_requested(sig);
}

int shutdown_init(void) {
  atomic_store(&g_shutdown_requested, 0);
  atomic_store(&g_shutdown_signal, 0);

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

  return 0;
}

void shutdown_cleanup(void) {
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = SIG_DFL;
  (void)sigaction(SIGINT, &sa, NULL);
  (void)sigaction(SIGTERM, &sa, NULL);
}

void shutdown_request(void) {
  shutdown_set_requested(0);
}

int shutdown_requested(void) {
  return atomic_load(&g_shutdown_requested) ? 1 : 0;
}

int shutdown_signal_number(void) {
  return atomic_load(&g_shutdown_signal);
}

int shutdown_exit_code(void) {
  return 130;
}
