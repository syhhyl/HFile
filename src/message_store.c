#include "message_store.h"

#include <ctype.h>
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
  int shutting_down;
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

static int message_store_is_unicode_whitespace(uint32_t cp) {
  if (cp <= 0x7Fu) {
    return isspace((unsigned char)cp) != 0;
  }

  return cp == 0x0085u || cp == 0x00A0u || cp == 0x1680u ||
         (cp >= 0x2000u && cp <= 0x200Au) || cp == 0x2028u ||
         cp == 0x2029u || cp == 0x202Fu || cp == 0x205Fu ||
         cp == 0x3000u;
}

static size_t message_store_trailing_whitespace_bytes(const char *message,
                                                      size_t len) {
  size_t seq_len = 0;
  uint32_t cp = 0;
  unsigned char b0 = 0;
  size_t start = 0;

  if (len == 0) {
    return 0;
  }

  if (((unsigned char)message[len - 1u] & 0x80u) == 0) {
    return message_store_is_unicode_whitespace(
             (unsigned char)message[len - 1u])
             ? 1u
             : 0u;
  }

  start = len - 1u;
  while (start > 0 &&
         (((unsigned char)message[start] & 0xC0u) == 0x80u)) {
    start--;
  }

  b0 = (unsigned char)message[start];
  if ((b0 & 0xE0u) == 0xC0u) {
    seq_len = 2u;
    cp = (uint32_t)(b0 & 0x1Fu);
  } else if ((b0 & 0xF0u) == 0xE0u) {
    seq_len = 3u;
    cp = (uint32_t)(b0 & 0x0Fu);
  } else if ((b0 & 0xF8u) == 0xF0u) {
    seq_len = 4u;
    cp = (uint32_t)(b0 & 0x07u);
  } else {
    return 0;
  }

  if (start + seq_len != len) {
    return 0;
  }

  for (size_t i = start + 1u; i < len; i++) {
    unsigned char bx = (unsigned char)message[i];
    if ((bx & 0xC0u) != 0x80u) {
      return 0;
    }
    cp = (cp << 6u) | (uint32_t)(bx & 0x3Fu);
  }

  if ((seq_len == 2u && cp < 0x80u) || (seq_len == 3u && cp < 0x800u) ||
      (seq_len == 4u && cp < 0x10000u) ||
      (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
    return 0;
  }

  return message_store_is_unicode_whitespace(cp) ? seq_len : 0u;
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
  g_message_store.shutting_down = 0;
  g_message_store.initialized = 1;
  return 0;
}

void message_store_shutdown(void) {
  if (!g_message_store.initialized) {
    return;
  }

  message_store_lock();
  g_message_store.shutting_down = 1;
#ifdef _WIN32
  WakeAllConditionVariable(&g_message_store.cond);
#else
  (void)pthread_cond_broadcast(&g_message_store.cond);
#endif
  message_store_unlock();
}

void message_store_cleanup(void) {
  if (!g_message_store.initialized) {
    return;
  }

  message_store_shutdown();
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
  while (len > 0) {
    size_t trimmed = message_store_trailing_whitespace_bytes(message, len);
    if (trimmed == 0) {
      break;
    }
    len -= trimmed;
  }
  copy = (char *)malloc(len + 1u);
  if (copy == NULL) {
    return 1;
  }
  if (len > 0) {
    memcpy(copy, message, len);
  }
  copy[len] = '\0';

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
    if (g_message_store.shutting_down) {
      break;
    }
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
