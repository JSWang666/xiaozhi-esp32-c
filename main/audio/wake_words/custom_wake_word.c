#include "wake_word.h"
#include "audio_codec.h"
#include "audio_internal.h"
#include "c_api/assets_c_api.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <cJSON.h>
#include <model_path.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <esp_audio_enc.h>

#define TAG "CustomWakeWord"
#define MAX_PCM_CHUNKS 70

typedef struct pcm_node {
    int16_t *samples;
    size_t count;
    struct pcm_node *next;
} pcm_node_t;

typedef struct {
    char *command;
    char *text;
    char *action;
} mn_cmd_t;

typedef struct {
    wake_word_t base;
    audio_codec_t *codec;
    srmodel_list_t *models;
    bool own_models;
    esp_mn_iface_t *multinet;
    model_iface_data_t *multinet_data;
    char *mn_name;
    mn_cmd_t *commands;
    int cmd_count;
    char language[16];
    int duration_ms;
    float threshold;
    volatile bool running;
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
    wake_word_detected_cb_t on_detected;
    void *on_ctx;
} cust_ww_t;

static void cust_free_commands(cust_ww_t *w)
{
    for (int i = 0; i < w->cmd_count; i++) {
        free(w->commands[i].command);
        free(w->commands[i].text);
        free(w->commands[i].action);
    }
    free(w->commands);
    w->commands = NULL;
    w->cmd_count = 0;
}

static void cust_free_pcm_list(pcm_node_t *head)
{
    while (head) {
        pcm_node_t *nx = head->next;
        free(head->samples);
        free(head);
        head = nx;
    }
}

static void cust_store_pcm(cust_ww_t *w, const int16_t *data, size_t samples)
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

static void cust_parse_json(cust_ww_t *w)
{
    strcpy(w->language, "cn");
    w->duration_ms = 3000;
    w->threshold = 0.2f;

    assets_handle_t *assets = assets_get_instance();
    if (!assets) {
        return;
    }
    void *ptr = NULL;
    size_t sz = 0;
    if (!assets_get_data(assets, "index.json", &ptr, &sz) || !ptr) {
        ESP_LOGE(TAG, "Failed to read index.json");
        return;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)ptr, sz);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse index.json");
        return;
    }
    cJSON *multinet_model = cJSON_GetObjectItem(root, "multinet_model");
    if (cJSON_IsObject(multinet_model)) {
        cJSON *language = cJSON_GetObjectItem(multinet_model, "language");
        cJSON *duration = cJSON_GetObjectItem(multinet_model, "duration");
        cJSON *threshold = cJSON_GetObjectItem(multinet_model, "threshold");
        cJSON *commands = cJSON_GetObjectItem(multinet_model, "commands");
        if (cJSON_IsString(language) && language->valuestring) {
            snprintf(w->language, sizeof(w->language), "%s", language->valuestring);
        }
        if (cJSON_IsNumber(duration)) {
            w->duration_ms = duration->valueint;
        }
        if (cJSON_IsNumber(threshold)) {
            w->threshold = (float)threshold->valuedouble;
        }
        if (cJSON_IsArray(commands)) {
            int n = cJSON_GetArraySize(commands);
            cust_free_commands(w);
            w->commands = (mn_cmd_t *)calloc((size_t)n, sizeof(mn_cmd_t));
            if (!w->commands) {
                cJSON_Delete(root);
                return;
            }
            int ci = 0;
            for (int i = 0; i < n; i++) {
                cJSON *command = cJSON_GetArrayItem(commands, i);
                if (!cJSON_IsObject(command)) {
                    continue;
                }
                cJSON *command_name = cJSON_GetObjectItem(command, "command");
                cJSON *text = cJSON_GetObjectItem(command, "text");
                cJSON *action = cJSON_GetObjectItem(command, "action");
                if (cJSON_IsString(command_name) && cJSON_IsString(text) && cJSON_IsString(action)) {
                    w->commands[ci].command = strdup(command_name->valuestring);
                    w->commands[ci].text = strdup(text->valuestring);
                    w->commands[ci].action = strdup(action->valuestring);
                    ESP_LOGI(TAG, "Command: %s, Text: %s, Action: %s", w->commands[ci].command,
                             w->commands[ci].text, w->commands[ci].action);
                    ci++;
                }
            }
            w->cmd_count = ci;
        }
    }
    cJSON_Delete(root);
}

static void cust_encode_task(void *arg)
{
    cust_ww_t *w = (cust_ww_t *)arg;
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
        cust_free_pcm_list(local);
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
                goto done_enc;
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
            }
            free(opus_buf);
            memmove(merge, merge + frame_samples, (merge_len - (size_t)frame_samples) * sizeof(int16_t));
            merge_len -= (size_t)frame_samples;
        }
    }
done_enc:
    cust_free_pcm_list(local);
    free(merge);
    esp_opus_enc_close(enc);
    ESP_LOGI(TAG, "Encode custom wake word opus %d packets in %ld ms", packets,
             (long)((esp_timer_get_time() - t0) / 1000));
    void *sentinel = NULL;
    xQueueSend(w->opus_q, &sentinel, portMAX_DELAY);
    vTaskDelete(NULL);
}

static void cust_set_cb(void *impl, wake_word_detected_cb_t cb, void *ctx)
{
    cust_ww_t *w = (cust_ww_t *)impl;
    w->on_detected = cb;
    w->on_ctx = ctx;
}

static bool cust_init(void *impl, void *codec, void *models)
{
    cust_ww_t *w = (cust_ww_t *)impl;
    w->codec = (audio_codec_t *)codec;
    srmodel_list_t *models_list = (srmodel_list_t *)models;

    cust_free_commands(w);
    if (models_list == NULL) {
        strcpy(w->language, "cn");
        w->models = esp_srmodel_init("model");
        w->own_models = true;
#ifdef CONFIG_CUSTOM_WAKE_WORD
        w->threshold = CONFIG_CUSTOM_WAKE_WORD_THRESHOLD / 100.0f;
        w->commands = (mn_cmd_t *)calloc(1, sizeof(mn_cmd_t));
        if (w->commands) {
            w->commands[0].command = strdup(CONFIG_CUSTOM_WAKE_WORD);
            w->commands[0].text = strdup(CONFIG_CUSTOM_WAKE_WORD_DISPLAY);
            w->commands[0].action = strdup("wake");
            w->cmd_count = 1;
        }
#endif
    } else {
        w->models = models_list;
        w->own_models = false;
        cust_parse_json(w);
    }

    if (w->models == NULL || w->models->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize multinet model");
        return false;
    }

    w->mn_name = esp_srmodel_filter(w->models, ESP_MN_PREFIX, w->language);
    if (w->mn_name == NULL) {
        ESP_LOGW(TAG, "Language '%s' multinet not found, falling back", w->language);
        w->mn_name = esp_srmodel_filter(w->models, ESP_MN_PREFIX, NULL);
    }
    if (w->mn_name == NULL) {
        ESP_LOGE(TAG, "Failed to initialize multinet");
        return false;
    }

    w->multinet = esp_mn_handle_from_name(w->mn_name);
    w->multinet_data = w->multinet->create(w->mn_name, w->duration_ms);
    w->multinet->set_det_threshold(w->multinet_data, w->threshold);
    esp_mn_commands_clear();
    for (int i = 0; i < w->cmd_count; i++) {
        esp_mn_commands_add(i + 1, w->commands[i].command);
    }
    esp_mn_commands_update();
    w->multinet->print_active_speech_commands(w->multinet_data);
    return true;
}

static void cust_feed(void *impl, const int16_t *data, size_t samples)
{
    cust_ww_t *w = (cust_ww_t *)impl;
    if (w->multinet_data == NULL || w->codec == NULL) {
        return;
    }
    if (xSemaphoreTake(w->input_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!w->running) {
        xSemaphoreGive(w->input_mutex);
        return;
    }

    size_t ch = (size_t)w->codec->input_channels;
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
    if (ch == 2) {
        for (size_t i = 0; i + 1 < samples; i += 2) {
            w->input_buf[w->input_len++] = data[i];
        }
    } else {
        memcpy(w->input_buf + w->input_len, data, samples * sizeof(int16_t));
        w->input_len += samples;
    }

    int chunksize = w->multinet->get_samp_chunksize(w->multinet_data);
    while ((int)w->input_len >= chunksize) {
        int16_t *chunk = (int16_t *)malloc((size_t)chunksize * sizeof(int16_t));
        if (!chunk) {
            break;
        }
        memcpy(chunk, w->input_buf, (size_t)chunksize * sizeof(int16_t));
        memmove(w->input_buf, w->input_buf + chunksize, (w->input_len - (size_t)chunksize) * sizeof(int16_t));
        w->input_len -= (size_t)chunksize;

        cust_store_pcm(w, chunk, (size_t)chunksize);

        esp_mn_state_t mn_state = w->multinet->detect(w->multinet_data, chunk);
        free(chunk);

        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = w->multinet->get_results(w->multinet_data);
            for (int i = 0; i < mn_result->num && w->running; i++) {
                ESP_LOGI(TAG, "Custom wake word detected: command_id=%d, string=%s, prob=%f", mn_result->command_id[i],
                         mn_result->string, mn_result->prob[i]);
                int cid = mn_result->command_id[i] - 1;
                if (cid >= 0 && cid < w->cmd_count && w->commands[cid].action &&
                    strcmp(w->commands[cid].action, "wake") == 0) {
                    snprintf(w->last_wake, sizeof(w->last_wake), "%s", w->commands[cid].text);
                    w->running = false;
                    w->input_len = 0;
                    if (w->on_detected) {
                        w->on_detected(w->last_wake, w->on_ctx);
                    }
                    break;
                }
            }
            w->multinet->clean(w->multinet_data);
        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            w->multinet->clean(w->multinet_data);
        }
        if (!w->running) {
            break;
        }
    }
    xSemaphoreGive(w->input_mutex);
}

static void cust_start(void *impl)
{
    cust_ww_t *w = (cust_ww_t *)impl;
    w->running = true;
}

static void cust_stop(void *impl)
{
    cust_ww_t *w = (cust_ww_t *)impl;
    w->running = false;
    if (xSemaphoreTake(w->input_mutex, portMAX_DELAY) == pdTRUE) {
        w->input_len = 0;
        xSemaphoreGive(w->input_mutex);
    }
}

static size_t cust_feed_size(void *impl)
{
    cust_ww_t *w = (cust_ww_t *)impl;
    if (w->multinet_data == NULL) {
        return 0;
    }
    return (size_t)w->multinet->get_samp_chunksize(w->multinet_data);
}

static void cust_encode_wake(void *impl)
{
    cust_ww_t *w = (cust_ww_t *)impl;
    const size_t stack_size = 4096 * 7;
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
    xTaskCreateStatic(cust_encode_task, "encode_wake_word", stack_size, w, 2, w->encode_stack, w->encode_tcb);
}

static bool cust_get_opus(void *impl, uint8_t *buf, size_t buf_size, size_t *out_len)
{
    cust_ww_t *w = (cust_ww_t *)impl;
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

static const char *cust_last(void *impl)
{
    cust_ww_t *w = (cust_ww_t *)impl;
    return w->last_wake;
}

static void cust_destroy(void *impl)
{
    cust_ww_t *w = (cust_ww_t *)impl;
    if (!w) {
        return;
    }
    if (w->multinet_data && w->multinet) {
        w->multinet->destroy(w->multinet_data);
    }
    if (w->own_models && w->models) {
        esp_srmodel_deinit(w->models);
    }
    cust_free_commands(w);
    free(w->input_buf);
    if (xSemaphoreTake(w->pcm_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cust_free_pcm_list(w->pcm_head);
        w->pcm_head = w->pcm_tail = NULL;
        xSemaphoreGive(w->pcm_mutex);
    }
    if (w->encode_stack) {
        heap_caps_free(w->encode_stack);
    }
    if (w->encode_tcb) {
        heap_caps_free(w->encode_tcb);
    }
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

static const wake_word_ops_t cust_ww_ops = {
    .initialize = cust_init,
    .feed = cust_feed,
    .set_detected_cb = cust_set_cb,
    .start = cust_start,
    .stop = cust_stop,
    .get_feed_size = cust_feed_size,
    .encode_wake_word_data = cust_encode_wake,
    .get_wake_word_opus = cust_get_opus,
    .get_last_detected = cust_last,
    .destroy = cust_destroy,
};

wake_word_t *custom_wake_word_create(void)
{
    cust_ww_t *w = (cust_ww_t *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->base.ops = &cust_ww_ops;
    w->input_mutex = xSemaphoreCreateMutex();
    w->pcm_mutex = xSemaphoreCreateMutex();
    w->opus_q = xQueueCreate(32, sizeof(void *));
    if (!w->input_mutex || !w->pcm_mutex || !w->opus_q) {
        cust_destroy(w);
        return NULL;
    }
    return &w->base;
}
