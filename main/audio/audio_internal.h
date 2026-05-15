#ifndef AUDIO_INTERNAL_H
#define AUDIO_INTERNAL_H

#include "audio_c_api.h"
#include <esp_audio_types.h>
#include <esp_opus_enc.h>
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY (1 << 3)

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                   \
    ((duration_ms) == 5 ? ESP_OPUS_ENC_FRAME_DURATION_5_MS :       \
     (duration_ms) == 10 ? ESP_OPUS_ENC_FRAME_DURATION_10_MS :   \
     (duration_ms) == 20 ? ESP_OPUS_ENC_FRAME_DURATION_20_MS :    \
     (duration_ms) == 40 ? ESP_OPUS_ENC_FRAME_DURATION_40_MS :    \
     (duration_ms) == 60 ? ESP_OPUS_ENC_FRAME_DURATION_60_MS :    \
     (duration_ms) == 80 ? ESP_OPUS_ENC_FRAME_DURATION_80_MS :     \
     (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS :   \
     (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS : -1)

#define AS_OPUS_ENC_CONFIG() {                                                                                    \
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,                                                                 \
        .channel = ESP_AUDIO_MONO,                                                                                 \
        .bits_per_sample = ESP_AUDIO_BIT16,                                                                        \
        .bitrate = ESP_OPUS_BITRATE_AUTO,                                                                          \
        .frame_duration = (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS),     \
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,                                                        \
        .complexity = 0,                                                                                           \
        .enable_fec = false,                                                                                       \
        .enable_dtx = true,                                                                                        \
        .enable_vbr = true,                                                                                        \
}

typedef enum {
    kAudioTaskTypeEncodeToSendQueue = 0,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
} audio_task_type_t;

#endif
