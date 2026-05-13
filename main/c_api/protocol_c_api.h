#ifndef PROTOCOL_C_API_H
#define PROTOCOL_C_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct protocol_handle protocol_handle_t;

typedef enum {
    PROTOCOL_KIND_MQTT = 0,
    PROTOCOL_KIND_WEBSOCKET,
} protocol_kind_t;

typedef enum {
    LISTEN_MODE_AUTOSTOP = 0,
    LISTEN_MODE_MANUALSTOP,
    LISTEN_MODE_REALTIME,
} listen_mode_t;

typedef struct {
    int sample_rate;
    int frame_duration;
    uint32_t timestamp;
    uint8_t *payload;
    size_t payload_size;
} protocol_audio_packet_t;

typedef void (*protocol_on_connected_fn)(void *user_ctx);
typedef void (*protocol_on_disconnected_fn)(void *user_ctx);
typedef void (*protocol_on_network_error_fn)(void *user_ctx, const char *message);
typedef void (*protocol_on_json_fn)(void *user_ctx, const char *json_text);
typedef void (*protocol_on_audio_fn)(void *user_ctx, const protocol_audio_packet_t *packet);

typedef struct {
    protocol_on_connected_fn on_connected;
    protocol_on_disconnected_fn on_disconnected;
    protocol_on_network_error_fn on_network_error;
    protocol_on_json_fn on_json;
    protocol_on_audio_fn on_audio;
    void *user_ctx;
} protocol_callbacks_t;

typedef struct {
    protocol_kind_t kind;
    protocol_callbacks_t callbacks;
} protocol_config_t;

protocol_handle_t *protocol_create(const protocol_config_t *cfg);
void protocol_destroy(protocol_handle_t *p);

esp_err_t protocol_start(protocol_handle_t *p);
esp_err_t protocol_open_audio_channel(protocol_handle_t *p);
esp_err_t protocol_close_audio_channel(protocol_handle_t *p, bool send_goodbye);
bool protocol_is_audio_channel_opened(const protocol_handle_t *p);

esp_err_t protocol_send_audio(protocol_handle_t *p, const protocol_audio_packet_t *packet);
esp_err_t protocol_send_wake_word_detected(protocol_handle_t *p, const char *wake_word);
esp_err_t protocol_send_start_listening(protocol_handle_t *p, listen_mode_t mode);
esp_err_t protocol_send_stop_listening(protocol_handle_t *p);
esp_err_t protocol_send_abort_speaking(protocol_handle_t *p, int reason);
esp_err_t protocol_send_mcp_message(protocol_handle_t *p, const char *message_json);

#ifdef __cplusplus
}
#endif

#endif
