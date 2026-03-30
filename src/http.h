#ifndef HF_HTTP_H
#define HF_HTTP_H

#include "cli.h"
#include "net.h"

int http_server(socket_t listener, const server_opt_t *ser_opt);
int http_handle_connection(socket_t conn, const server_opt_t *ser_opt);

#endif  // HF_HTTP_H
