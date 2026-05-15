#include "node_conn_tracker.h"

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

struct node_conn_entry_t {
  socket_t conn;
  int closing;
  struct node_conn_entry_t *next;
};

typedef struct {
  int initialized;
  int shutting_down;
  unsigned int active_count;
  node_conn_entry_t *head;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} node_conn_tracker_t;

static node_conn_tracker_t g_node_conn_tracker = {0};

int node_conn_tracker_init(void) {
  if (g_node_conn_tracker.initialized) {
    return 0;
  }

  if (pthread_mutex_init(&g_node_conn_tracker.mutex, NULL) != 0) {
    return 1;
  }
  if (pthread_cond_init(&g_node_conn_tracker.cond, NULL) != 0) {
    (void)pthread_mutex_destroy(&g_node_conn_tracker.mutex);
    return 1;
  }

  g_node_conn_tracker.initialized = 1;
  g_node_conn_tracker.shutting_down = 0;
  g_node_conn_tracker.active_count = 0;
  g_node_conn_tracker.head = NULL;
  return 0;
}

static void node_conn_tracker_abort_entry(node_conn_entry_t *entry) {
  if (entry == NULL || entry->closing) {
    return;
  }
  entry->closing = 1;
  (void)shutdown(entry->conn, SHUT_RDWR);
}

node_conn_entry_t *node_conn_tracker_begin(socket_t conn) {
  node_conn_entry_t *entry = (node_conn_entry_t *)malloc(sizeof(*entry));
  if (entry == NULL) {
    return NULL;
  }

  entry->conn = conn;
  entry->closing = 0;
  entry->next = NULL;

  (void)pthread_mutex_lock(&g_node_conn_tracker.mutex);
  if (g_node_conn_tracker.shutting_down) {
    (void)pthread_mutex_unlock(&g_node_conn_tracker.mutex);
    free(entry);
    return NULL;
  }

  entry->next = g_node_conn_tracker.head;
  g_node_conn_tracker.head = entry;
  g_node_conn_tracker.active_count++;
  (void)pthread_mutex_unlock(&g_node_conn_tracker.mutex);
  return entry;
}

void node_conn_tracker_end(node_conn_entry_t *entry) {
  if (entry == NULL) {
    return;
  }

  (void)pthread_mutex_lock(&g_node_conn_tracker.mutex);

  node_conn_entry_t **link = &g_node_conn_tracker.head;
  while (*link != NULL) {
    if (*link == entry) {
      *link = entry->next;
      break;
    }
    link = &(*link)->next;
  }
  if (g_node_conn_tracker.active_count > 0) {
    g_node_conn_tracker.active_count--;
  }
  if (g_node_conn_tracker.shutting_down && g_node_conn_tracker.active_count == 0) {
    (void)pthread_cond_broadcast(&g_node_conn_tracker.cond);
  }

  (void)pthread_mutex_unlock(&g_node_conn_tracker.mutex);

  free(entry);
}

void node_conn_tracker_shutdown_all(void) {
  if (!g_node_conn_tracker.initialized) {
    return;
  }

  (void)pthread_mutex_lock(&g_node_conn_tracker.mutex);
  g_node_conn_tracker.shutting_down = 1;
  for (node_conn_entry_t *entry = g_node_conn_tracker.head;
       entry != NULL;
       entry = entry->next) {
    node_conn_tracker_abort_entry(entry);
  }
  if (g_node_conn_tracker.active_count == 0) {
    (void)pthread_cond_broadcast(&g_node_conn_tracker.cond);
  }
  (void)pthread_mutex_unlock(&g_node_conn_tracker.mutex);
}

void node_conn_tracker_wait_idle(void) {
  if (!g_node_conn_tracker.initialized) {
    return;
  }

  (void)pthread_mutex_lock(&g_node_conn_tracker.mutex);
  while (g_node_conn_tracker.active_count > 0) {
    (void)pthread_cond_wait(&g_node_conn_tracker.cond,
                            &g_node_conn_tracker.mutex);
  }
  (void)pthread_mutex_unlock(&g_node_conn_tracker.mutex);
}

void node_conn_tracker_cleanup(void) {
  if (!g_node_conn_tracker.initialized) {
    return;
  }

  (void)pthread_cond_destroy(&g_node_conn_tracker.cond);
  (void)pthread_mutex_destroy(&g_node_conn_tracker.mutex);

  g_node_conn_tracker.initialized = 0;
  g_node_conn_tracker.shutting_down = 0;
  g_node_conn_tracker.active_count = 0;
  g_node_conn_tracker.head = NULL;
}
