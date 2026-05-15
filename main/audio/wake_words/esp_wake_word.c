#include "wake_word.h"
#include "audio_codec.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <model_path.h>
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define TAG "EspWakeWord"

typedef struct {
    wake_word_t base;
    audio_codec_t *codec;
    srmodel_list_t *models;
    bool own_models;
    esp_wn_iface_t *wakenet_iface;
    model_iface_data_t *wakenet_data;
    srmodel_list_t *wakenet_model;
    volatile bool running;
    int16_t *input_buf;
    size_t input_len;
    size_t input_cap;
    SemaphoreHandle_t buf_mutex;
    char last_wake[64];
    wake_word_detected_cb_t on_detected;
    void *on_ctx;
} esp_ww_t;

static void esp_ww_set_cb(void *impl, wake_word_detected_cb_t cb, void *ctx)
{
    esp_ww_t *w = (esp_ww_t *)impl;
    w->on_detected = cb;
    w->on_ctx = ctx;
}

static bool esp_ww_init(void *impl, void *codec, void *models)
{
    esp_ww_t *w = (esp_ww_t *)impl;
    w->codec = (audio_codec_t *)codec;
    srmodel_list_t *models_list = (srmodel_list_t *)models;

    if (models_list == NULL) {
        w->wakenet_model = esp_srmodel_init("model");
        w->own_models = true;
    } else {
        w->wakenet_model = models_list;
        w->own_models = false;
    }

    if (w->wakenet_model == NULL || w->wakenet_model->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }
    if (w->wakenet_model->num > 1) {
        ESP_LOGW(TAG, "More than one model found, using the first one");
    } else if (w->wakenet_model->num == 0) {
        ESP_LOGE(TAG, "No model found");
        return false;
    }
    char *model_name = w->wakenet_model->model_name[0];
    w->wakenet_iface = (esp_wn_iface_t *)esp_wn_handle_from_name(model_name);
    w->wakenet_data = w->wakenet_iface->create(model_name, DET_MODE_95);

    int frequency = w->wakenet_iface->get_samp_rate(w->wakenet_data);
    int audio_chunksize = w->wakenet_iface->get_samp_chunksize(w->wakenet_data);
    ESP_LOGI(TAG, "Wake word(%s),freq: %d, chunksize: %d", model_name, frequency, audio_chunksize);
    return true;
}

static void esp_ww_feed(void *impl, const int16_t *data, size_t samples)
{
    esp_ww_t *w = (esp_ww_t *)impl;
    if (w->wakenet_data == NULL || w->codec == NULL) {
        return;
    }
    if (xSemaphoreTake(w->buf_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!w->running) {
        xSemaphoreGive(w->buf_mutex);
        return;
    }

    if (w->codec->input_channels == 2) {
        for (size_t i = 0; i + 1 < samples; i += 2) {
            if (w->input_len >= w->input_cap) {
                size_t ncap = w->input_cap ? w->input_cap * 2 : 1024;
                int16_t *nb = (int16_t *)realloc(w->input_buf, ncap * sizeof(int16_t));
                if (!nb) {
                    xSemaphoreGive(w->buf_mutex);
                    return;
                }
                w->input_buf = nb;
                w->input_cap = ncap;
            }
            w->input_buf[w->input_len++] = data[i];
        }
    } else {
        size_t need = w->input_len + samples;
        if (need > w->input_cap) {
            size_t ncap = w->input_cap ? w->input_cap * 2 : need;
            while (ncap < need) {
                ncap *= 2;
            }
            int16_t *nb = (int16_t *)realloc(w->input_buf, ncap * sizeof(int16_t));
            if (!nb) {
                xSemaphoreGive(w->buf_mutex);
                return;
            }
            w->input_buf = nb;
            w->input_cap = ncap;
        }
        memcpy(w->input_buf + w->input_len, data, samples * sizeof(int16_t));
        w->input_len += samples;
    }

    int chunksize = w->wakenet_iface->get_samp_chunksize(w->wakenet_data);
    while ((int)w->input_len >= chunksize) {
        int res = w->wakenet_iface->detect(w->wakenet_data, w->input_buf);
        if (res > 0) {
            const char *name = w->wakenet_iface->get_word_name(w->wakenet_data, res);
            snprintf(w->last_wake, sizeof(w->last_wake), "%s", name ? name : "");
            w->running = false;
            w->input_len = 0;
            if (w->on_detected) {
                w->on_detected(w->last_wake, w->on_ctx);
            }
            xSemaphoreGive(w->buf_mutex);
            return;
        }
        memmove(w->input_buf, w->input_buf + chunksize,
                (w->input_len - (size_t)chunksize) * sizeof(int16_t));
        w->input_len -= (size_t)chunksize;
    }
    xSemaphoreGive(w->buf_mutex);
}

static void esp_ww_start(void *impl)
{
    esp_ww_t *w = (esp_ww_t *)impl;
    w->running = true;
}

static void esp_ww_stop(void *impl)
{
    esp_ww_t *w = (esp_ww_t *)impl;
    w->running = false;
    if (xSemaphoreTake(w->buf_mutex, portMAX_DELAY) == pdTRUE) {
        w->input_len = 0;
        xSemaphoreGive(w->buf_mutex);
    }
}

static size_t esp_ww_feed_size(void *impl)
{
    esp_ww_t *w = (esp_ww_t *)impl;
    if (w->wakenet_data == NULL) {
        return 0;
    }
    return (size_t)w->wakenet_iface->get_samp_chunksize(w->wakenet_data);
}

static void esp_ww_encode(void *impl)
{
    (void)impl;
}

static bool esp_ww_get_opus(void *impl, uint8_t *buf, size_t buf_size, size_t *out_len)
{
    (void)impl;
    (void)buf;
    (void)buf_size;
    if (out_len) {
        *out_len = 0;
    }
    return false;
}

static const char *esp_ww_last(void *impl)
{
    esp_ww_t *w = (esp_ww_t *)impl;
    return w->last_wake;
}

static void esp_ww_destroy(void *impl)
{
    esp_ww_t *w = (esp_ww_t *)impl;
    if (!w) {
        return;
    }
    if (w->wakenet_data != NULL && w->wakenet_iface != NULL) {
        w->wakenet_iface->destroy(w->wakenet_data);
    }
    if (w->own_models && w->wakenet_model != NULL) {
        esp_srmodel_deinit(w->wakenet_model);
    }
    free(w->input_buf);
    if (w->buf_mutex) {
        vSemaphoreDelete(w->buf_mutex);
    }
    free(w);
}

static const wake_word_ops_t esp_ww_ops = {
    .initialize = esp_ww_init,
    .feed = esp_ww_feed,
    .set_detected_cb = esp_ww_set_cb,
    .start = esp_ww_start,
    .stop = esp_ww_stop,
    .get_feed_size = esp_ww_feed_size,
    .encode_wake_word_data = esp_ww_encode,
    .get_wake_word_opus = esp_ww_get_opus,
    .get_last_detected = esp_ww_last,
    .destroy = esp_ww_destroy,
};

wake_word_t *esp_wake_word_create(void)
{
    esp_ww_t *w = (esp_ww_t *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->base.ops = &esp_ww_ops;
    w->buf_mutex = xSemaphoreCreateMutex();
    if (!w->buf_mutex) {
        free(w);
        return NULL;
    }
    return &w->base;
}
