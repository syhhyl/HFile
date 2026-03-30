#ifndef HF_MESSAGE_STORE_H
#define HF_MESSAGE_STORE_H

#include <stdint.h>

int message_store_init(void);
void message_store_shutdown(void);
void message_store_cleanup(void);
int message_store_set(const char *message);
int message_store_get_copy(char **message_out, int *has_message_out);
int message_store_get_snapshot(char **message_out, int *has_message_out,
                               uint64_t *version_out);
int message_store_wait_for_update(uint64_t known_version,
                                  uint32_t timeout_ms,
                                  char **message_out,
                                  int *has_message_out,
                                  uint64_t *version_out);

#endif  // HF_MESSAGE_STORE_H
