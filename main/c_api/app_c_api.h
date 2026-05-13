#ifndef APP_C_API_H
#define APP_C_API_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_context app_context_t;

typedef enum {
    APP_EVENT_SCHEDULE = 0,
    APP_EVENT_SEND_AUDIO,
    APP_EVENT_WAKE_WORD_DETECTED,
    APP_EVENT_VAD_CHANGE,
    APP_EVENT_ERROR,
    APP_EVENT_ACTIVATION_DONE,
    APP_EVENT_CLOCK_TICK,
    APP_EVENT_NETWORK_CONNECTED,
    APP_EVENT_NETWORK_DISCONNECTED,
    APP_EVENT_TOGGLE_CHAT,
    APP_EVENT_START_LISTENING,
    APP_EVENT_STOP_LISTENING,
    APP_EVENT_STATE_CHANGED,
} app_event_t;

typedef void (*app_task_fn)(void *arg);

typedef struct {
    bool enable_device_aec;
    bool enable_server_aec;
} app_config_t;

app_context_t *app_get_context(void);
app_context_t *app_create(const app_config_t *cfg);
void app_destroy(app_context_t *ctx);

esp_err_t app_init(app_context_t *ctx);
esp_err_t app_run(app_context_t *ctx);

esp_err_t app_post_event(app_context_t *ctx, app_event_t event, const void *event_data);
esp_err_t app_schedule(app_context_t *ctx, app_task_fn task, void *arg);

esp_err_t app_toggle_chat(app_context_t *ctx);
esp_err_t app_start_listening(app_context_t *ctx);
esp_err_t app_stop_listening(app_context_t *ctx);
esp_err_t app_reboot(app_context_t *ctx);
esp_err_t app_abort_speaking(app_context_t *ctx, int reason);
esp_err_t app_alert(app_context_t *ctx, const char *status, const char *message, const char *emotion, const char *sound);
esp_err_t app_dismiss_alert(app_context_t *ctx);
esp_err_t app_play_sound(app_context_t *ctx, const char *sound_name);
int app_get_device_state(app_context_t *ctx);
bool app_set_device_state(app_context_t *ctx, int state);
bool app_is_voice_detected(app_context_t *ctx);
bool app_can_enter_sleep_mode(app_context_t *ctx);
esp_err_t app_wake_word_invoke(app_context_t *ctx, const char *wake_word);
esp_err_t app_send_mcp_message(app_context_t *ctx, const char *payload);
esp_err_t app_set_aec_mode(app_context_t *ctx, int mode);
int app_get_aec_mode(app_context_t *ctx);
esp_err_t app_reset_protocol(app_context_t *ctx);
bool app_upgrade_firmware(app_context_t *ctx, const char *url, const char *version);

#ifdef __cplusplus
}
#endif

#endif
