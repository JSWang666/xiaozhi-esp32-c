#include "wake_word.h"
#include "audio_codec.h"
#include "audio_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <model_path.h>
#include <esp_afe_sr_models.h>
#include <esp_nsn_models.h>
#include <esp_audio_enc.h>

#define TAG "AfeWakeWord"
#define DETECTION_RUNNING_EVENT 1
#define MAX_PCM_CHUNKS 70

typedef struct pcm_node {
    int16_t *samples;
    size_t count;
    struct pcm_node *next;
} pcm_node_t;

typedef struct {
    wake_word_t base;
    audio_codec_t *codec;
    srmodel_list_t *models;
    bool own_models;
    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t *afe_data;
    char *wakenet_model_name;
    char **wake_words;
    int wake_word_count;
    EventGroupHandle_t event_group;
    wake_word_detected_cb_t on_detected;
    void *on_ctx;
    int16_t *input_buf;
    size_t input_len;
    size_t input_cap;
    SemaphoreHandle_t input_mutex;
    pcm_node_t *pcm_head;
    pcm_node_t *pcm_tail;
    size_t pcm_chunks;
    SemaphoreHandle_t pcm_mutex;
    StackType_t *encode_stack;
    StaticTask_t *encode_tcb;
    QueueHandle_t opus_q;
    char last_wake[128];
} afe_ww_t;

static void afe_free_pcm_list(pcm_node_t *head)
{
    while (head) {
        pcm_node_t *nx = head->next;
        free(head->samples);
        free(head);
        head = nx;
    }
}

static void afe_store_pcm(afe_ww_t *w, const int16_t *data, size_t samples)
{
    if (xSemaphoreTake(w->pcm_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    pcm_node_t *n = (pcm_node_t *)malloc(sizeof(pcm_node_t));
    if (!n) {
        xSemaphoreGive(w->pcm_mutex);
        return;
    }
    n->samples = (int16_t *)malloc(samples * sizeof(int16_t));
    if (!n->samples) {
        free(n);
        xSemaphoreGive(w->pcm_mutex);
        return;
    }
    memcpy(n->samples, data, samples * sizeof(int16_t));
    n->count = samples;
    n->next = NULL;
    if (w->pcm_tail) {
        w->pcm_tail->next = n;
    } else {
        w->pcm_head = n;
    }
    w->pcm_tail = n;
    w->pcm_chunks++;
    while (w->pcm_chunks > MAX_PCM_CHUNKS) {
        pcm_node_t *h = w->pcm_head;
        if (!h) {
            break;
        }
        w->pcm_head = h->next;
        if (!w->pcm_head) {
            w->pcm_tail = NULL;
        }
        free(h->samples);
        free(h);
        w->pcm_chunks--;
    }
    xSemaphoreGive(w->pcm_mutex);
}

static void afe_detection_task(void *arg)
{
    afe_ww_t *w = (afe_ww_t *)arg;
    if (!w->afe_iface || !w->afe_data) {
        vTaskDelete(NULL);
        return;
    }
    int feed_sz = w->afe_iface->get_feed_chunksize(w->afe_data);
    int fetch_sz = w->afe_iface->get_fetch_chunksize(w->afe_data);
    ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d", feed_sz, fetch_sz);

    while (1) {
        xEventGroupWaitBits(w->event_group, DETECTION_RUNNING_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);
        afe_fetch_result_t *res = w->afe_iface->fetch_with_delay(w->afe_data, portMAX_DELAY);
        if (res == NULL || res->ret_value == ESP_FAIL) {
            continue;
        }
        afe_store_pcm(w, (const int16_t *)res->data, res->data_size / sizeof(int16_t));

        if (res->wakeup_state == WAKENET_DETECTED) {
            xEventGroupClearBits(w->event_group, DETECTION_RUNNING_EVENT);
            int idx = res->wakenet_model_index - 1;
            if (idx >= 0 && idx < w->wake_word_count && w->wake_words[idx]) {
                snprintf(w->last_wake, sizeof(w->last_wake), "%s", w->wake_words[idx]);
            } else {
                w->last_wake[0] = '\0';
            }
            if (w->on_detected) {
                w->on_detected(w->last_wake, w->on_ctx);
            }
        }
    }
}

static void afe_encode_task(void *arg)
{
    afe_ww_t *w = (afe_ww_t *)arg;
    int64_t t0 = esp_timer_get_time();

    pcm_node_t *local = NULL;
    if (xSemaphoreTake(w->pcm_mutex, portMAX_DELAY) == pdTRUE) {
        local = w->pcm_head;
        w->pcm_head = w->pcm_tail = NULL;
        w->pcm_chunks = 0;
        xSemaphoreGive(w->pcm_mutex);
    }

    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    void *enc = NULL;
    esp_err_t er = esp_opus_enc_open(&opus_enc_cfg, sizeof(opus_enc_cfg), &enc);
    if (enc == NULL) {
        ESP_LOGE(TAG, "Failed to create audio encoder: %d", (int)er);
        void *sentinel = NULL;
        xQueueSend(w->opus_q, &sentinel, portMAX_DELAY);
        afe_free_pcm_list(local);
        vTaskDelete(NULL);
        return;
    }
    int frame_bytes = 0;
    int outbuf_size = 0;
    esp_opus_enc_get_frame_size(enc, &frame_bytes, &outbuf_size);
    int frame_samples = frame_bytes / (int)sizeof(int16_t);

    int packets = 0;
    int16_t *merge = NULL;
    size_t merge_len = 0;
    size_t merge_cap = 0;

    for (pcm_node_t *n = local; n; n = n->next) {
        size_t need = merge_len + n->count;
        if (need > merge_cap) {
            size_t nc = merge_cap ? merge_cap * 2 : need;
            while (nc < need) {
                nc *= 2;
            }
            int16_t *nm = (int16_t *)realloc(merge, nc * sizeof(int16_t));
            if (!nm) {
                break;
            }
            merge = nm;
            merge_cap = nc;
        }
        memcpy(merge + merge_len, n->samples, n->count * sizeof(int16_t));
        merge_len += n->count;

        while ((int)merge_len >= frame_samples) {
            uint8_t *opus_buf = (uint8_t *)malloc((size_t)outbuf_size);
            if (!opus_buf) {
                goto enc_done;
            }
            esp_audio_enc_in_frame_t in = {
                .buffer = (uint8_t *)merge,
                .len = (uint32_t)(frame_samples * sizeof(int16_t)),
            };
            esp_audio_enc_out_frame_t out = {
                .buffer = opus_buf,
                .len = (uint32_t)outbuf_size,
                .encoded_bytes = 0,
            };
            er = esp_opus_enc_process(enc, &in, &out);
            if (er == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
                uint8_t *block = (uint8_t *)malloc(sizeof(size_t) + out.encoded_bytes);
                if (block) {
                    *(size_t *)block = out.encoded_bytes;
                    memcpy(block + sizeof(size_t), opus_buf, out.encoded_bytes);
                    void *sendp = block;
                    xQueueSend(w->opus_q, &sendp, portMAX_DELAY);
                    packets++;
                }
            } else {
                ESP_LOGE(TAG, "opus enc fail %d", (int)er);
            }
            free(opus_buf);
            memmove(merge, merge + frame_samples, (merge_len - (size_t)frame_samples) * sizeof(int16_t));
            merge_len -= (size_t)frame_samples;
        }
    }
enc_done:
    afe_free_pcm_list(local);
    free(merge);
    esp_opus_enc_close(enc);
    ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets,
             (long)((esp_timer_get_time() - t0) / 1000));
    void *sentinel = NULL;
    xQueueSend(w->opus_q, &sentinel, portMAX_DELAY);
    vTaskDelete(NULL);
}

static void afe_set_cb(void *impl, wake_word_detected_cb_t cb, void *ctx)
{
    afe_ww_t *w = (afe_ww_t *)impl;
    w->on_detected = cb;
    w->on_ctx = ctx;
}

static bool afe_init(void *impl, void *codec, void *models)
{
    afe_ww_t *w = (afe_ww_t *)impl;
    w->codec = (audio_codec_t *)codec;
    srmodel_list_t *models_list = (srmodel_list_t *)models;
    int ref_num = (w->codec && w->codec->input_reference) ? 1 : 0;
    int mic_ch = w->codec ? (w->codec->input_channels - ref_num) : 1;
    if (mic_ch < 1) {
        mic_ch = 1;
    }

    if (models_list == NULL) {
        w->models = esp_srmodel_init("model");
        w->own_models = true;
    } else {
        w->models = models_list;
        w->own_models = false;
    }
    if (w->models == NULL || w->models->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }

    w->wakenet_model_name = NULL;
    for (int i = 0; i < w->models->num; i++) {
        if (strstr(w->models->model_name[i], ESP_WN_PREFIX) != NULL) {
            w->wakenet_model_name = w->models->model_name[i];
            const char *words = esp_srmodel_get_wake_words(w->models, w->wakenet_model_name);
            int cnt = 1;
            for (const char *p = words; p && *p; p++) {
                if (*p == ';') {
                    cnt++;
                }
            }
            w->wake_words = (char **)calloc((size_t)cnt, sizeof(char *));
            if (!w->wake_words) {
                return false;
            }
            char *tmp = strdup(words ? words : "");
            if (!tmp) {
                return false;
            }
            int wi = 0;
            char *save = NULL;
            for (char *tok = strtok_r(tmp, ";", &save); tok && wi < cnt; tok = strtok_r(NULL, ";", &save)) {
                while (*tok == ' ') {
                    tok++;
                }
                w->wake_words[wi++] = strdup(tok);
            }
            w->wake_word_count = wi;
            free(tmp);
            break;
        }
    }

    char input_format[16] = {0};
    int p = 0;
    for (int i = 0; i < mic_ch && p < (int)sizeof(input_format) - 1; i++) {
        input_format[p++] = 'M';
    }
    for (int i = 0; i < ref_num && p < (int)sizeof(input_format) - 1; i++) {
        input_format[p++] = 'R';
    }
    input_format[p] = '\0';

    afe_config_t *afe_config = afe_config_init(input_format, w->models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = w->codec && w->codec->input_reference;
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    w->afe_iface = esp_afe_handle_from_config(afe_config);
    w->afe_data = w->afe_iface->create_from_config(afe_config);

    xTaskCreate(afe_detection_task, "audio_detection", 4096, w, 3, NULL);
    return true;
}

static void afe_feed(void *impl, const int16_t *data, size_t samples)
{
    afe_ww_t *w = (afe_ww_t *)impl;
    if (w->afe_data == NULL || w->codec == NULL) {
        return;
    }
    if (xSemaphoreTake(w->input_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!(xEventGroupGetBits(w->event_group) & DETECTION_RUNNING_EVENT)) {
        xSemaphoreGive(w->input_mutex);
        return;
    }
    size_t ch = (size_t)w->codec->input_channels;
    size_t chunk = (size_t)w->afe_iface->get_feed_chunksize(w->afe_data) * ch;
    size_t need = w->input_len + samples;
    if (need > w->input_cap) {
        size_t nc = w->input_cap ? w->input_cap * 2 : need;
        while (nc < need) {
            nc *= 2;
        }
        int16_t *nb = (int16_t *)realloc(w->input_buf, nc * sizeof(int16_t));
        if (!nb) {
            xSemaphoreGive(w->input_mutex);
            return;
        }
        w->input_buf = nb;
        w->input_cap = nc;
    }
    memcpy(w->input_buf + w->input_len, data, samples * sizeof(int16_t));
    w->input_len += samples;
    while (w->input_len >= chunk) {
        w->afe_iface->feed(w->afe_data, w->input_buf);
        memmove(w->input_buf, w->input_buf + chunk, (w->input_len - chunk) * sizeof(int16_t));
        w->input_len -= chunk;
    }
    xSemaphoreGive(w->input_mutex);
}

static void afe_start(void *impl)
{
    afe_ww_t *w = (afe_ww_t *)impl;
    xEventGroupSetBits(w->event_group, DETECTION_RUNNING_EVENT);
}

static void afe_stop(void *impl)
{
    afe_ww_t *w = (afe_ww_t *)impl;
    xEventGroupClearBits(w->event_group, DETECTION_RUNNING_EVENT);
    if (xSemaphoreTake(w->input_mutex, portMAX_DELAY) == pdTRUE) {
        if (w->afe_data) {
            w->afe_iface->reset_buffer(w->afe_data);
        }
        w->input_len = 0;
        xSemaphoreGive(w->input_mutex);
    }
}

static size_t afe_feed_size(void *impl)
{
    afe_ww_t *w = (afe_ww_t *)impl;
    if (w->afe_data == NULL) {
        return 0;
    }
    return (size_t)w->afe_iface->get_feed_chunksize(w->afe_data);
}

static void afe_encode_wake(void *impl)
{
    afe_ww_t *w = (afe_ww_t *)impl;
    const size_t stack_size = 4096 * 6;
    void *tmp = NULL;
    while (xQueueReceive(w->opus_q, &tmp, 0) == pdTRUE) {
        if (tmp) {
            free(tmp);
        }
    }
    if (w->encode_stack == NULL) {
        w->encode_stack = (StackType_t *)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
    }
    if (w->encode_tcb == NULL) {
        w->encode_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    }
    if (!w->encode_stack || !w->encode_tcb) {
        return;
    }
    xTaskCreateStatic(afe_encode_task, "encode_wake_word", stack_size, w, 2, w->encode_stack, w->encode_tcb);
}

static bool afe_get_opus(void *impl, uint8_t *buf, size_t buf_size, size_t *out_len)
{
    afe_ww_t *w = (afe_ww_t *)impl;
    void *ptr = NULL;
    if (xQueueReceive(w->opus_q, &ptr, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (ptr == NULL) {
        if (out_len) {
            *out_len = 0;
        }
        return false;
    }
    uint8_t *block = (uint8_t *)ptr;
    size_t len = *(size_t *)block;
    size_t copy = len < buf_size ? len : buf_size;
    if (buf && copy) {
        memcpy(buf, block + sizeof(size_t), copy);
    }
    free(block);
    if (out_len) {
        *out_len = copy;
    }
    return copy > 0;
}

static const char *afe_last(void *impl)
{
    afe_ww_t *w = (afe_ww_t *)impl;
    return w->last_wake;
}

static void afe_destroy(void *impl)
{
    afe_ww_t *w = (afe_ww_t *)impl;
    if (!w) {
        return;
    }
    if (w->afe_data && w->afe_iface) {
        w->afe_iface->destroy(w->afe_data);
    }
    if (w->own_models && w->models) {
        esp_srmodel_deinit(w->models);
    }
    for (int i = 0; i < w->wake_word_count; i++) {
        free(w->wake_words[i]);
    }
    free(w->wake_words);
    free(w->input_buf);
    if (xSemaphoreTake(w->pcm_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        afe_free_pcm_list(w->pcm_head);
        w->pcm_head = w->pcm_tail = NULL;
        xSemaphoreGive(w->pcm_mutex);
    }
    if (w->encode_stack) {
        heap_caps_free(w->encode_stack);
    }
    if (w->encode_tcb) {
        heap_caps_free(w->encode_tcb);
    }
    vEventGroupDelete(w->event_group);
    if (w->input_mutex) {
        vSemaphoreDelete(w->input_mutex);
    }
    if (w->pcm_mutex) {
        vSemaphoreDelete(w->pcm_mutex);
    }
    if (w->opus_q) {
        void *p = NULL;
        while (xQueueReceive(w->opus_q, &p, 0) == pdTRUE) {
            if (p) {
                free(p);
            }
        }
        vQueueDelete(w->opus_q);
    }
    free(w);
}

static const wake_word_ops_t afe_ww_ops = {
    .initialize = afe_init,
    .feed = afe_feed,
    .set_detected_cb = afe_set_cb,
    .start = afe_start,
    .stop = afe_stop,
    .get_feed_size = afe_feed_size,
    .encode_wake_word_data = afe_encode_wake,
    .get_wake_word_opus = afe_get_opus,
    .get_last_detected = afe_last,
    .destroy = afe_destroy,
};

wake_word_t *afe_wake_word_create(void)
{
    afe_ww_t *w = (afe_ww_t *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->base.ops = &afe_ww_ops;
    w->event_group = xEventGroupCreate();
    w->input_mutex = xSemaphoreCreateMutex();
    w->pcm_mutex = xSemaphoreCreateMutex();
    w->opus_q = xQueueCreate(32, sizeof(void *));
    if (!w->event_group || !w->input_mutex || !w->pcm_mutex || !w->opus_q) {
        afe_destroy(w);
        return NULL;
    }
    return &w->base;
}
