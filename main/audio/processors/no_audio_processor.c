#include "audio_processor.h"
#include "audio_codec.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define TAG "NoAudioProcessor"

typedef struct {
    audio_processor_t base;
    audio_codec_t *codec;
    int frame_samples;
    int16_t *output_buf;
    size_t output_len;
    size_t output_cap;
    audio_processor_output_cb_t output_cb;
    void *output_ctx;
    audio_processor_vad_cb_t vad_cb;
    void *vad_ctx;
    bool running;
} no_audio_processor_t;

static void nap_initialize(void *impl, void *codec_ptr, int frame_duration_ms, void *models)
{
    no_audio_processor_t *p = (no_audio_processor_t *)impl;
    p->codec = (audio_codec_t *)codec_ptr;
    p->frame_samples = frame_duration_ms * 16000 / 1000;
    p->output_cap = (size_t)p->frame_samples * 2;
    p->output_buf = (int16_t *)malloc(p->output_cap * sizeof(int16_t));
    p->output_len = 0;
}

static void nap_feed(void *impl, const int16_t *data, size_t samples)
{
    no_audio_processor_t *p = (no_audio_processor_t *)impl;
    if (!p->running || !p->output_cb) return;

    int channels = p->codec ? p->codec->input_channels : 1;
    if (channels == 2) {
        for (size_t i = 0; i < samples / 2; i++) {
            if (p->output_len < p->output_cap) {
                p->output_buf[p->output_len++] = data[i * 2];
            }
        }
    } else {
        size_t to_copy = samples;
        if (p->output_len + to_copy > p->output_cap) {
            to_copy = p->output_cap - p->output_len;
        }
        memcpy(p->output_buf + p->output_len, data, to_copy * sizeof(int16_t));
        p->output_len += to_copy;
    }

    while (p->output_len >= (size_t)p->frame_samples) {
        p->output_cb(p->output_buf, (size_t)p->frame_samples, p->output_ctx);
        size_t remaining = p->output_len - (size_t)p->frame_samples;
        if (remaining > 0) {
            memmove(p->output_buf, p->output_buf + p->frame_samples, remaining * sizeof(int16_t));
        }
        p->output_len = remaining;
    }
}

static void nap_start(void *impl)
{
    no_audio_processor_t *p = (no_audio_processor_t *)impl;
    p->running = true;
}

static void nap_stop(void *impl)
{
    no_audio_processor_t *p = (no_audio_processor_t *)impl;
    p->running = false;
    p->output_len = 0;
}

static bool nap_is_running(void *impl)
{
    no_audio_processor_t *p = (no_audio_processor_t *)impl;
    return p->running;
}

static void nap_set_output_cb(void *impl, audio_processor_output_cb_t cb, void *ctx)
{
    no_audio_processor_t *p = (no_audio_processor_t *)impl;
    p->output_cb = cb;
    p->output_ctx = ctx;
}

static void nap_set_vad_cb(void *impl, audio_processor_vad_cb_t cb, void *ctx)
{
    no_audio_processor_t *p = (no_audio_processor_t *)impl;
    p->vad_cb = cb;
    p->vad_ctx = ctx;
}

static size_t nap_get_feed_size(void *impl)
{
    no_audio_processor_t *p = (no_audio_processor_t *)impl;
    if (!p->codec) return 0;
    return (size_t)p->frame_samples;
}

static void nap_enable_device_aec(void *impl, bool enable)
{
    if (enable) {
        ESP_LOGE(TAG, "Device AEC is not supported");
    }
}

static void nap_destroy(void *impl)
{
    no_audio_processor_t *p = (no_audio_processor_t *)impl;
    if (p) {
        free(p->output_buf);
        free(p);
    }
}

static const audio_processor_ops_t no_audio_processor_ops = {
    .initialize = nap_initialize,
    .feed = nap_feed,
    .start = nap_start,
    .stop = nap_stop,
    .is_running = nap_is_running,
    .set_output_cb = nap_set_output_cb,
    .set_vad_cb = nap_set_vad_cb,
    .get_feed_size = nap_get_feed_size,
    .enable_device_aec = nap_enable_device_aec,
    .destroy = nap_destroy,
};

audio_processor_t *no_audio_processor_create(void)
{
    no_audio_processor_t *p = (no_audio_processor_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->base.ops = &no_audio_processor_ops;
    return &p->base;
}
