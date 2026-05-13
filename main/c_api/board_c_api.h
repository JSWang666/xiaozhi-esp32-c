#ifndef BOARD_C_API_H
#define BOARD_C_API_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct board_handle board_handle_t;

typedef enum {
    BOARD_NET_SCANNING = 0,
    BOARD_NET_CONNECTING,
    BOARD_NET_CONNECTED,
    BOARD_NET_DISCONNECTED,
    BOARD_NET_WIFI_CONFIG_MODE_ENTER,
    BOARD_NET_WIFI_CONFIG_MODE_EXIT,
    BOARD_NET_MODEM_DETECTING,
    BOARD_NET_MODEM_ERROR_NO_SIM,
    BOARD_NET_MODEM_ERROR_REG_DENIED,
    BOARD_NET_MODEM_ERROR_INIT_FAILED,
    BOARD_NET_MODEM_ERROR_TIMEOUT,
} board_network_event_t;

typedef enum {
    BOARD_POWER_LOW = 0,
    BOARD_POWER_BALANCED,
    BOARD_POWER_PERFORMANCE,
} board_power_level_t;

typedef void (*board_network_event_cb)(void *user_ctx, board_network_event_t event, const char *data);

typedef struct {
    board_network_event_cb cb;
    void *user_ctx;
} board_network_callback_t;

typedef struct {
    const char *board_type;
} board_init_cfg_t;

board_handle_t *board_get_instance(void);
board_handle_t *board_create(const board_init_cfg_t *cfg);
void board_destroy(board_handle_t *board);

const char *board_get_type(board_handle_t *board);
const char *board_get_uuid(board_handle_t *board);

void *board_get_audio_codec(board_handle_t *board);
void *board_get_display(board_handle_t *board);
void *board_get_led(board_handle_t *board);
void *board_get_backlight(board_handle_t *board);
void *board_get_camera(board_handle_t *board);
void *board_get_network(board_handle_t *board);

esp_err_t board_start_network(board_handle_t *board);
esp_err_t board_set_network_callback(board_handle_t *board, const board_network_callback_t *cb);
esp_err_t board_set_power_level(board_handle_t *board, board_power_level_t level);

/* Trigger Wi-Fi provisioning mode if the underlying Board is a WifiBoard
 * derivative. Returns ESP_ERR_NOT_SUPPORTED on non-Wi-Fi boards. */
esp_err_t board_enter_wifi_config_mode(board_handle_t *board);

const char *board_get_network_state_icon(board_handle_t *board);
bool board_get_battery_level(board_handle_t *board, int *level, bool *charging, bool *discharging);
bool board_get_temperature(board_handle_t *board, float *temperature);
const char *board_get_system_info_json(board_handle_t *board);
const char *board_get_board_json(board_handle_t *board);
const char *board_get_device_status_json(board_handle_t *board);

#ifdef __cplusplus
}
#endif

#endif
