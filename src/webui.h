#ifndef HF_WEBUI_H
#define HF_WEBUI_H

#include <stddef.h>

typedef struct {
  const char *path;
  const char *content_type;
  const char *body;
  size_t body_len;
} webui_asset_t;

const webui_asset_t *webui_find_asset(const char *path);

#endif  // HF_WEBUI_H
