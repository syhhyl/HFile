#include "server_conn_tracker.h"

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

struct server_conn_entry_t {
  socket_t conn;
  int closing;
  struct server_conn_entry_t *next;
};

typedef struct {
  int initialized;
  int shutting_down;
  unsigned int active_count;
  server_conn_entry_t *head;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} server_conn_tracker_t;

static server_conn_tracker_t g_server_conn_tracker = {0};

int server_conn_tracker_init(void) {
  if (g_server_conn_tracker.initialized) {
    return 0;
  }

  if (pthread_mutex_init(&g_server_conn_tracker.mutex, NULL) != 0) {
    return 1;
  }
  if (pthread_cond_init(&g_server_conn_tracker.cond, NULL) != 0) {
    (void)pthread_mutex_destroy(&g_server_conn_tracker.mutex);
    return 1;
  }

  g_server_conn_tracker.initialized = 1;
  g_server_conn_tracker.shutting_down = 0;
  g_server_conn_tracker.active_count = 0;
  g_server_conn_tracker.head = NULL;
  return 0;
}

static void server_conn_tracker_abort_entry(server_conn_entry_t *entry) {
  if (entry == NULL || entry->closing) {
    return;
  }
  entry->closing = 1;
  (void)shutdown(entry->conn, SHUT_RDWR);
}

server_conn_entry_t *server_conn_tracker_begin(socket_t conn) {
  server_conn_entry_t *entry = (server_conn_entry_t *)malloc(sizeof(*entry));
  if (entry == NULL) {
    return NULL;
  }

  entry->conn = conn;
  entry->closing = 0;
  entry->next = NULL;

  (void)pthread_mutex_lock(&g_server_conn_tracker.mutex);
  if (g_server_conn_tracker.shutting_down) {
    (void)pthread_mutex_unlock(&g_server_conn_tracker.mutex);
    free(entry);
    return NULL;
  }

  entry->next = g_server_conn_tracker.head;
  g_server_conn_tracker.head = entry;
  g_server_conn_tracker.active_count++;
  (void)pthread_mutex_unlock(&g_server_conn_tracker.mutex);
  return entry;
}

void server_conn_tracker_end(server_conn_entry_t *entry) {
  if (entry == NULL) {
    return;
  }

  (void)pthread_mutex_lock(&g_server_conn_tracker.mutex);

  server_conn_entry_t **link = &g_server_conn_tracker.head;
  while (*link != NULL) {
    if (*link == entry) {
      *link = entry->next;
      break;
    }
    link = &(*link)->next;
  }
  if (g_server_conn_tracker.active_count > 0) {
    g_server_conn_tracker.active_count--;
  }
  if (g_server_conn_tracker.shutting_down && g_server_conn_tracker.active_count == 0) {
    (void)pthread_cond_broadcast(&g_server_conn_tracker.cond);
  }

  (void)pthread_mutex_unlock(&g_server_conn_tracker.mutex);

  free(entry);
}

void server_conn_tracker_shutdown_all(void) {
  if (!g_server_conn_tracker.initialized) {
    return;
  }

  (void)pthread_mutex_lock(&g_server_conn_tracker.mutex);
  g_server_conn_tracker.shutting_down = 1;
  for (server_conn_entry_t *entry = g_server_conn_tracker.head;
       entry != NULL;
       entry = entry->next) {
    server_conn_tracker_abort_entry(entry);
  }
  if (g_server_conn_tracker.active_count == 0) {
    (void)pthread_cond_broadcast(&g_server_conn_tracker.cond);
  }
  (void)pthread_mutex_unlock(&g_server_conn_tracker.mutex);
}

void server_conn_tracker_wait_idle(void) {
  if (!g_server_conn_tracker.initialized) {
    return;
  }

  (void)pthread_mutex_lock(&g_server_conn_tracker.mutex);
  while (g_server_conn_tracker.active_count > 0) {
    (void)pthread_cond_wait(&g_server_conn_tracker.cond,
                            &g_server_conn_tracker.mutex);
  }
  (void)pthread_mutex_unlock(&g_server_conn_tracker.mutex);
}

void server_conn_tracker_cleanup(void) {
  if (!g_server_conn_tracker.initialized) {
    return;
  }

  (void)pthread_cond_destroy(&g_server_conn_tracker.cond);
  (void)pthread_mutex_destroy(&g_server_conn_tracker.mutex);

  g_server_conn_tracker.initialized = 0;
  g_server_conn_tracker.shutting_down = 0;
  g_server_conn_tracker.active_count = 0;
  g_server_conn_tracker.head = NULL;
}
