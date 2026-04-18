#ifndef HF_CONTROL_H
#define HF_CONTROL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int control_status(void);
int control_stop(void);
int control_build_url(uint16_t port,
                      char *url_out,
                      size_t url_out_cap,
                      int *phone_reachable_out);
void control_print_server_access_details(FILE *out,
                                         const char *receive_dir,
                                         uint16_t port,
                                         const char *log_path,
                                         long pid,
                                         int daemon_mode);

#endif  // HF_CONTROL_H
