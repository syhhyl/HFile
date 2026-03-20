#include "message_store.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <pthread.h>
  #include <time.h>
#endif

typedef struct {
  char *message;
  uint64_t version;
  int initialized;
#ifdef _WIN32
  CRITICAL_SECTION mutex;
  CONDITION_VARIABLE cond;
#else
  pthread_mutex_t mutex;
  pthread_cond_t cond;
#endif
} message_store_state_t;

static message_store_state_t g_message_store = {0};

static void message_store_lock(void) {
#ifdef _WIN32
  EnterCriticalSection(&g_message_store.mutex);
#else
  (void)pthread_mutex_lock(&g_message_store.mutex);
#endif
}

static void message_store_unlock(void) {
#ifdef _WIN32
  LeaveCriticalSection(&g_message_store.mutex);
#else
  (void)pthread_mutex_unlock(&g_message_store.mutex);
#endif
}

int message_store_init(void) {
  if (g_message_store.initialized) {
    return 0;
  }

#ifdef _WIN32
  InitializeCriticalSection(&g_message_store.mutex);
  InitializeConditionVariable(&g_message_store.cond);
#else
  if (pthread_mutex_init(&g_message_store.mutex, NULL) != 0) {
    return 1;
  }
  if (pthread_cond_init(&g_message_store.cond, NULL) != 0) {
    (void)pthread_mutex_destroy(&g_message_store.mutex);
    return 1;
  }
#endif

  g_message_store.message = NULL;
  g_message_store.version = 0;
  g_message_store.initialized = 1;
  return 0;
}

void message_store_cleanup(void) {
  if (!g_message_store.initialized) {
    return;
  }

  message_store_lock();
  if (g_message_store.message != NULL) {
    free(g_message_store.message);
    g_message_store.message = NULL;
  }
  message_store_unlock();

#ifdef _WIN32
  DeleteCriticalSection(&g_message_store.mutex);
#else
  (void)pthread_cond_destroy(&g_message_store.cond);
  (void)pthread_mutex_destroy(&g_message_store.mutex);
#endif

  g_message_store.initialized = 0;
}

int message_store_set(const char *message) {
  size_t len = 0;
  char *copy = NULL;

  if (!g_message_store.initialized || message == NULL) {
    return 1;
  }

  len = strlen(message);
  copy = (char *)malloc(len + 1u);
  if (copy == NULL) {
    return 1;
  }
  memcpy(copy, message, len + 1u);

  message_store_lock();
  if (g_message_store.message != NULL) {
    free(g_message_store.message);
  }
  g_message_store.message = copy;
  g_message_store.version++;
#ifdef _WIN32
  WakeAllConditionVariable(&g_message_store.cond);
#else
  (void)pthread_cond_broadcast(&g_message_store.cond);
#endif
  message_store_unlock();
  return 0;
}

int message_store_get_copy(char **message_out, int *has_message_out) {
  return message_store_get_snapshot(message_out, has_message_out, NULL);
}

int message_store_get_snapshot(char **message_out, int *has_message_out,
                               uint64_t *version_out) {
  char *copy = NULL;
  const char *src = NULL;
  size_t len = 0;

  if (!g_message_store.initialized || message_out == NULL ||
      has_message_out == NULL) {
    return 1;
  }

  *message_out = NULL;
  *has_message_out = 0;

  message_store_lock();
  src = g_message_store.message;
  if (src != NULL) {
    len = strlen(src);
    copy = (char *)malloc(len + 1u);
    if (copy != NULL) {
      memcpy(copy, src, len + 1u);
    }
  }
  if (version_out != NULL) {
    *version_out = g_message_store.version;
  }
  message_store_unlock();

  if (src != NULL && copy == NULL) {
    return 1;
  }

  *message_out = copy;
  *has_message_out = src != NULL ? 1 : 0;
  return 0;
}

int message_store_wait_for_update(uint64_t known_version,
                                  uint32_t timeout_ms,
                                  char **message_out,
                                  int *has_message_out,
                                  uint64_t *version_out) {
  char *copy = NULL;
  const char *src = NULL;
  size_t len = 0;
  int updated = 0;

  if (!g_message_store.initialized || message_out == NULL ||
      has_message_out == NULL || version_out == NULL) {
    return 1;
  }

  *message_out = NULL;
  *has_message_out = 0;
  *version_out = known_version;

  message_store_lock();

  while (g_message_store.version <= known_version) {
#ifdef _WIN32
    if (!SleepConditionVariableCS(&g_message_store.cond, &g_message_store.mutex,
                                  timeout_ms)) {
      if (GetLastError() == ERROR_TIMEOUT) {
        break;
      }
      message_store_unlock();
      return 1;
    }
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
      message_store_unlock();
      return 1;
    }
    ts.tv_sec += (time_t)(timeout_ms / 1000u);
    ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000L;
    }
    int err = pthread_cond_timedwait(&g_message_store.cond,
                                     &g_message_store.mutex, &ts);
    if (err != 0 && err != ETIMEDOUT) {
      message_store_unlock();
      return 1;
    }
    if (err == ETIMEDOUT) {
      break;
    }
#endif
  }

  if (g_message_store.version > known_version) {
    updated = 1;
    src = g_message_store.message;
    if (src != NULL) {
      len = strlen(src);
      copy = (char *)malloc(len + 1u);
      if (copy != NULL) {
        memcpy(copy, src, len + 1u);
      }
    }
  }
  *version_out = g_message_store.version;
  message_store_unlock();

  if (updated && src != NULL && copy == NULL) {
    return 1;
  }

  *message_out = copy;
  *has_message_out = updated && src != NULL ? 1 : 0;
  return 0;
}
