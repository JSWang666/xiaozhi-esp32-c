#ifndef OTA_C_API_H
#define OTA_C_API_H

#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ota_handle ota_handle_t;
typedef void (*ota_progress_cb_t)(int progress, size_t speed, void *ctx);

ota_handle_t *ota_create(void);
void ota_destroy(ota_handle_t *ota);

esp_err_t ota_check_version(ota_handle_t *ota);
esp_err_t ota_activate(ota_handle_t *ota);
void ota_mark_current_version_valid(ota_handle_t *ota);

bool ota_has_new_version(ota_handle_t *ota);
bool ota_has_mqtt_config(ota_handle_t *ota);
bool ota_has_websocket_config(ota_handle_t *ota);
bool ota_has_activation_code(ota_handle_t *ota);
bool ota_has_activation_challenge(ota_handle_t *ota);
bool ota_has_server_time(ota_handle_t *ota);

const char *ota_get_firmware_version(ota_handle_t *ota);
const char *ota_get_current_version(ota_handle_t *ota);
const char *ota_get_firmware_url(ota_handle_t *ota);
const char *ota_get_activation_message(ota_handle_t *ota);
const char *ota_get_activation_code(ota_handle_t *ota);
const char *ota_get_check_version_url(ota_handle_t *ota);

bool ota_start_upgrade(ota_handle_t *ota, ota_progress_cb_t cb, void *ctx);
bool ota_upgrade_from_url(const char *url, ota_progress_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
