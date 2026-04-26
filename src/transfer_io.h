#ifndef HF_TRANSFER_IO_H
#define HF_TRANSFER_IO_H

#include "net.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

#define STACK_BUF_SIZE 8192u
#define HEAP_BUF_SIZE (256u * 1024u)
#define HEAP_THRESHOLD (1u * 1024u * 1024u)

protocol_result_t transfer_recv_socket_file(socket_t conn,
                                            const char *base_dir,
                                            const char *file_name,
                                            uint64_t content_size,
                                            const char *recv_ctx,
                                            const char *short_read_message,
                                            char *full_path_out,
                                            size_t full_path_cap);

protocol_result_t transfer_recv_socket_http_file(socket_t conn,
                                                 const char *base_dir,
                                                 const char *file_name,
                                                 uint64_t content_size,
                                                 const char *recv_ctx,
                                                 const char *short_read_message,
                                                 char *full_path_out,
                                                 size_t full_path_cap);

#endif  // HF_TRANSFER_IO_H
