#include "audio_processor.h"
#include "audio_codec.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_afe_sr_models.h>
#include <esp_nsn_models.h>
#include <model_path.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

#define TAG "AfeAudioProcessor"
#define PROCESSOR_RUNNING 0x01

typedef struct {
    audio_processor_t base;
    EventGroupHandle_t event_group;
    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t *afe_data;
    audio_codec_t *codec;
    int frame_samples;
    bool is_speaking;
    int16_t *input_buf;
    size_t input_len;
    size_t input_cap;
    SemaphoreHandle_t input_mutex;
    int16_t *out_buf;
    size_t out_len;
    size_t out_cap;
    audio_processor_output_cb_t out_cb;
    void *out_ctx;
    audio_processor_vad_cb_t vad_cb;
    void *vad_ctx;
} afe_ap_t;

static void afe_ap_task(void *arg)
{
    afe_ap_t *p = (afe_ap_t *)arg;
    if (!p->afe_iface || !p->afe_data) {
        vTaskDelete(NULL);
        return;
    }
    int feed_sz = p->afe_iface->get_feed_chunksize(p->afe_data);
    int fetch_sz = p->afe_iface->get_fetch_chunksize(p->afe_data);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d", feed_sz, fetch_sz);

    while (1) {
        xEventGroupWaitBits(p->event_group, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);
        afe_fetch_result_t *res = p->afe_iface->fetch_with_delay(p->afe_data, portMAX_DELAY);
        if ((xEventGroupGetBits(p->event_group) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == NULL || res->ret_value == ESP_FAIL) {
            if (res != NULL) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }

        if (p->vad_cb) {
            if (res->vad_state == VAD_SPEECH && !p->is_speaking) {
                p->is_speaking = true;
                p->vad_cb(true, p->vad_ctx);
            } else if (res->vad_state == VAD_SILENCE && p->is_speaking) {
                p->is_speaking = false;
                p->vad_cb(false, p->vad_ctx);
            }
        }

        if (p->out_cb) {
            size_t samples = res->data_size / sizeof(int16_t);
            size_t need = p->out_len + samples;
            if (need > p->out_cap) {
                size_t nc = p->out_cap ? p->out_cap * 2 : need;
                while (nc < need) {
                    nc *= 2;
                }
                int16_t *nb = (int16_t *)realloc(p->out_buf, nc * sizeof(int16_t));
                if (!nb) {
                    continue;
                }
                p->out_buf = nb;
                p->out_cap = nc;
            }
            memcpy(p->out_buf + p->out_len, res->data, samples * sizeof(int16_t));
            p->out_len += samples;

            while (p->out_len >= (size_t)p->frame_samples) {
                int16_t frame_stk[1536];
                if (p->frame_samples > (int)(sizeof(frame_stk) / sizeof(frame_stk[0]))) {
                    break;
                }
                memcpy(frame_stk, p->out_buf, (size_t)p->frame_samples * sizeof(int16_t));
                memmove(p->out_buf, p->out_buf + p->frame_samples,
                        (p->out_len - (size_t)p->frame_samples) * sizeof(int16_t));
                p->out_len -= (size_t)p->frame_samples;
                if (p->out_cb) {
                    p->out_cb(frame_stk, (size_t)p->frame_samples, p->out_ctx);
                }
            }
        }
    }
}

static void afe_init(void *impl, void *codec_ptr, int frame_duration_ms, void *models)
{
    afe_ap_t *p = (afe_ap_t *)impl;
    p->codec = (audio_codec_t *)codec_ptr;
    p->frame_samples = frame_duration_ms * 16000 / 1000;

    int ref_num = (p->codec && p->codec->input_reference) ? 1 : 0;
    int mic_ch = p->codec ? (p->codec->input_channels - ref_num) : 1;
    if (mic_ch < 1) {
        mic_ch = 1;
    }

    srmodel_list_t *models_list = (srmodel_list_t *)models;
    srmodel_list_t *models_use = models_list;
    bool free_models = false;
    if (models_use == NULL) {
        models_use = esp_srmodel_init("model");
        free_models = true;
    }

    char input_format[16] = {0};
    int pos = 0;
    for (int i = 0; i < mic_ch && pos < (int)sizeof(input_format) - 1; i++) {
        input_format[pos++] = 'M';
    }
    for (int i = 0; i < ref_num && pos < (int)sizeof(input_format) - 1; i++) {
        input_format[pos++] = 'R';
    }
    input_format[pos] = '\0';

    char *ns_model_name = esp_srmodel_filter(models_use, ESP_NSNET_PREFIX, NULL);
    char *vad_model_name = esp_srmodel_filter(models_use, ESP_VADN_PREFIX, NULL);

    afe_config_t *afe_config = afe_config_init(input_format, NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != NULL) {
        afe_config->vad_model_name = vad_model_name;
    }
    if (ns_model_name != NULL) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }
    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = false;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    p->afe_iface = esp_afe_handle_from_config(afe_config);
    p->afe_data = p->afe_iface->create_from_config(afe_config);

    if (free_models && models_use) {
        esp_srmodel_deinit(models_use);
    }

    xTaskCreate(afe_ap_task, "audio_communication", 4096, p, 3, NULL);
}

static void afe_feed(void *impl, const int16_t *data, size_t samples)
{
    afe_ap_t *p = (afe_ap_t *)impl;
    if (p->afe_data == NULL || p->codec == NULL) {
        return;
    }
    if (xSemaphoreTake(p->input_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!(xEventGroupGetBits(p->event_group) & PROCESSOR_RUNNING)) {
        xSemaphoreGive(p->input_mutex);
        return;
    }
    size_t ch = (size_t)p->codec->input_channels;
    size_t chunk = (size_t)p->afe_iface->get_feed_chunksize(p->afe_data) * ch;
    size_t need = p->input_len + samples;
    if (need > p->input_cap) {
        size_t nc = p->input_cap ? p->input_cap * 2 : need;
        while (nc < need) {
            nc *= 2;
        }
        int16_t *nb = (int16_t *)realloc(p->input_buf, nc * sizeof(int16_t));
        if (!nb) {
            xSemaphoreGive(p->input_mutex);
            return;
        }
        p->input_buf = nb;
        p->input_cap = nc;
    }
    memcpy(p->input_buf + p->input_len, data, samples * sizeof(int16_t));
    p->input_len += samples;
    while (p->input_len >= chunk) {
        p->afe_iface->feed(p->afe_data, p->input_buf);
        memmove(p->input_buf, p->input_buf + chunk, (p->input_len - chunk) * sizeof(int16_t));
        p->input_len -= chunk;
    }
    xSemaphoreGive(p->input_mutex);
}

static void afe_start(void *impl)
{
    afe_ap_t *p = (afe_ap_t *)impl;
    xEventGroupSetBits(p->event_group, PROCESSOR_RUNNING);
}

static void afe_stop(void *impl)
{
    afe_ap_t *p = (afe_ap_t *)impl;
    xEventGroupClearBits(p->event_group, PROCESSOR_RUNNING);
    if (xSemaphoreTake(p->input_mutex, portMAX_DELAY) == pdTRUE) {
        if (p->afe_data) {
            p->afe_iface->reset_buffer(p->afe_data);
        }
        p->input_len = 0;
        xSemaphoreGive(p->input_mutex);
    }
}

static bool afe_is_running(void *impl)
{
    afe_ap_t *p = (afe_ap_t *)impl;
    return (xEventGroupGetBits(p->event_group) & PROCESSOR_RUNNING) != 0;
}

static void afe_set_out(void *impl, audio_processor_output_cb_t cb, void *ctx)
{
    afe_ap_t *p = (afe_ap_t *)impl;
    p->out_cb = cb;
    p->out_ctx = ctx;
}

static void afe_set_vad(void *impl, audio_processor_vad_cb_t cb, void *ctx)
{
    afe_ap_t *p = (afe_ap_t *)impl;
    p->vad_cb = cb;
    p->vad_ctx = ctx;
}

static size_t afe_feed_size(void *impl)
{
    afe_ap_t *p = (afe_ap_t *)impl;
    if (p->afe_data == NULL) {
        return 0;
    }
    return (size_t)p->afe_iface->get_feed_chunksize(p->afe_data);
}

static void afe_enable_aec(void *impl, bool enable)
{
    afe_ap_t *p = (afe_ap_t *)impl;
    if (!p->afe_iface || !p->afe_data) {
        return;
    }
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        p->afe_iface->disable_vad(p->afe_data);
        p->afe_iface->enable_aec(p->afe_data);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        p->afe_iface->disable_aec(p->afe_data);
        p->afe_iface->enable_vad(p->afe_data);
    }
}

static void afe_destroy(void *impl)
{
    afe_ap_t *p = (afe_ap_t *)impl;
    if (!p) {
        return;
    }
    if (p->afe_data && p->afe_iface) {
        p->afe_iface->destroy(p->afe_data);
    }
    vEventGroupDelete(p->event_group);
    free(p->input_buf);
    free(p->out_buf);
    if (p->input_mutex) {
        vSemaphoreDelete(p->input_mutex);
    }
    free(p);
}

static const audio_processor_ops_t afe_ap_ops = {
    .initialize = afe_init,
    .feed = afe_feed,
    .start = afe_start,
    .stop = afe_stop,
    .is_running = afe_is_running,
    .set_output_cb = afe_set_out,
    .set_vad_cb = afe_set_vad,
    .get_feed_size = afe_feed_size,
    .enable_device_aec = afe_enable_aec,
    .destroy = afe_destroy,
};

audio_processor_t *afe_audio_processor_create(void)
{
    afe_ap_t *p = (afe_ap_t *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->base.ops = &afe_ap_ops;
    p->event_group = xEventGroupCreate();
    p->input_mutex = xSemaphoreCreateMutex();
    if (!p->event_group || !p->input_mutex) {
        afe_destroy(p);
        return NULL;
    }
    return &p->base;
}
