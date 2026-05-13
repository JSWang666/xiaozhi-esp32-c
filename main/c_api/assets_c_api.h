#ifndef ASSETS_C_API_H
#define ASSETS_C_API_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct assets_handle assets_handle_t;
typedef void (*assets_progress_cb_t)(int progress, size_t speed, void *ctx);

assets_handle_t *assets_get_instance(void);
bool assets_partition_valid(const assets_handle_t *assets);
const char *assets_default_url(const assets_handle_t *assets);

bool assets_download(assets_handle_t *assets, const char *url,
                     assets_progress_cb_t cb, void *ctx);
bool assets_apply(assets_handle_t *assets, bool refresh_display_theme);
bool assets_get_data(assets_handle_t *assets, const char *name,
                     void **ptr, size_t *size);

#ifdef __cplusplus
}
#endif

#endif
