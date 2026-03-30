#ifndef HF_SHUTDOWN_H
#define HF_SHUTDOWN_H

int shutdown_init(void);
void shutdown_cleanup(void);
void shutdown_request(void);
int shutdown_requested(void);
int shutdown_signal_number(void);
int shutdown_exit_code(void);

#endif  // HF_SHUTDOWN_H
