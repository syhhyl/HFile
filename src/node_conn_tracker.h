#ifndef HF_NODE_CONN_TRACKER_H
#define HF_NODE_CONN_TRACKER_H

#include "net.h"

typedef struct node_conn_entry_t node_conn_entry_t;

int node_conn_tracker_init(void);
node_conn_entry_t *node_conn_tracker_begin(socket_t conn);
void node_conn_tracker_end(node_conn_entry_t *entry);
void node_conn_tracker_shutdown_all(void);
void node_conn_tracker_wait_idle(void);
void node_conn_tracker_cleanup(void);

#endif  // HF_NODE_CONN_TRACKER_H
