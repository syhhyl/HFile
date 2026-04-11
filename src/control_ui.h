#ifndef HF_CONTROL_UI_H
#define HF_CONTROL_UI_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "daemon_state.h"

int control_ui_is_tty_stdout(void);
int control_ui_build_url(uint16_t port,
                         char *url_out,
                         size_t url_out_cap,
                         int *phone_reachable_out);
int control_ui_print_qr(FILE *out, const char *url);
void control_ui_print_server_access_details(FILE *out,
                                            const char *receive_dir,
                                            uint16_t port,
                                            const char *log_path,
                                            long pid,
                                            int daemon_mode);
void control_ui_print_status(FILE *out, const daemon_state_t *state);

#endif  // HF_CONTROL_UI_H
