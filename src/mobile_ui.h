#ifndef HF_MOBILE_UI_H
#define HF_MOBILE_UI_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

int mobile_ui_is_tty_stdout(void);
int mobile_ui_build_url(uint16_t port,
                        char *url_out,
                        size_t url_out_cap,
                        int *phone_reachable_out);
int mobile_ui_print_qr(FILE *out, const char *url);

#endif  // HF_MOBILE_UI_H
