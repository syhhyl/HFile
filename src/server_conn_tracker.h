#ifndef HF_SERVER_CONN_TRACKER_H
#define HF_SERVER_CONN_TRACKER_H

#include "net.h"

typedef struct server_conn_entry_t server_conn_entry_t;

int server_conn_tracker_init(void);
server_conn_entry_t *server_conn_tracker_begin(socket_t conn);
void server_conn_tracker_end(server_conn_entry_t *entry);
void server_conn_tracker_shutdown_all(void);
void server_conn_tracker_wait_idle(void);
void server_conn_tracker_cleanup(void);

#endif  // HF_SERVER_CONN_TRACKER_H
