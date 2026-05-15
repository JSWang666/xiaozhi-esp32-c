#ifndef AUDIO_C_API_H
#define AUDIO_C_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifndef OPUS_FRAME_DURATION_MS
#define OPUS_FRAME_DURATION_MS 60
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_service audio_service_t;

typedef struct {
    int sample_rate;
    int frame_duration_ms;
    uint32_t timestamp;
    uint8_t *payload;
    size_t payload_size;
} audio_packet_t;

typedef void (*audio_on_send_queue_available_fn)(void *user_ctx);
typedef void (*audio_on_wake_word_fn)(void *user_ctx, const char *wake_word);
typedef void (*audio_on_vad_change_fn)(void *user_ctx, bool speaking);

typedef struct {
    audio_on_send_queue_available_fn on_send_queue_available;
    audio_on_wake_word_fn on_wake_word;
    audio_on_vad_change_fn on_vad_change;
    void *user_ctx;
} audio_callbacks_t;

typedef struct {
    bool enable_wake_word;
    bool enable_voice_processing;
} audio_service_cfg_t;

audio_service_t *audio_service_get_instance(void);
audio_service_t *audio_service_create(const audio_service_cfg_t *cfg);
void audio_service_destroy(audio_service_t *svc);

esp_err_t audio_service_init(audio_service_t *svc, void *audio_codec_handle);
esp_err_t audio_service_start(audio_service_t *svc);
esp_err_t audio_service_stop(audio_service_t *svc);

esp_err_t audio_service_set_callbacks(audio_service_t *svc, const audio_callbacks_t *callbacks);

esp_err_t audio_service_enable_wake_word(audio_service_t *svc, bool enable);
esp_err_t audio_service_enable_voice_processing(audio_service_t *svc, bool enable);
esp_err_t audio_service_enable_audio_testing(audio_service_t *svc, bool enable);

bool audio_service_is_idle(audio_service_t *svc);
bool audio_service_is_voice_detected(const audio_service_t *svc);
const char *audio_service_get_last_wake_word(audio_service_t *svc);

esp_err_t audio_service_push_decode_packet(audio_service_t *svc, const audio_packet_t *packet);
esp_err_t audio_service_pop_send_packet(audio_service_t *svc, audio_packet_t *out_packet);
esp_err_t audio_service_pop_wake_word_packet(audio_service_t *svc, audio_packet_t *out_packet);
void audio_service_free_packet(audio_packet_t *packet);

esp_err_t audio_service_play_ogg(audio_service_t *svc, const uint8_t *data, size_t len);

/** @return number of int16 samples written to @p out, or 0 on failure */
size_t audio_service_read_audio_data(audio_service_t *svc, int16_t *out, size_t out_cap_int16, int sample_rate,
                                     int samples_per_ch);

esp_err_t audio_service_reset_decoder(audio_service_t *svc);

esp_err_t audio_service_encode_wake_word(audio_service_t *svc);
esp_err_t audio_service_enable_device_aec(audio_service_t *svc, bool enable);
void audio_service_wait_for_playback_empty(audio_service_t *svc);
bool audio_service_is_wake_word_running(const audio_service_t *svc);
bool audio_service_is_audio_processor_running(const audio_service_t *svc);
bool audio_service_is_afe_wake_word(audio_service_t *svc);
esp_err_t audio_service_set_models_list(audio_service_t *svc, void *models);

#ifdef __cplusplus
}
#endif

#endif
