#ifndef HF_HTTP_H
#define HF_HTTP_H

#include "cli.h"
#include "net.h"

int handle_http_connection(socket_t conn, const server_opt_t *ser_opt);

#endif  // HF_HTTP_H
