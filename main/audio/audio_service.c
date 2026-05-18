/*
 * Pure C audio service (migrated from audio_service.cc).
 */
#include "audio_c_api.h"
#include "audio_internal.h"
#include "audio_codec.h"
#include "audio_processor.h"
#include "wake_word.h"
#include "demuxer/ogg_demuxer.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_audio_enc.h>
#include <esp_audio_dec.h>
#include <esp_opus_enc.h>
#include <esp_opus_dec.h>
#include <esp_ae_rate_cvt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#if CONFIG_USE_AUDIO_DEBUGGER
#include "processors/audio_debugger.h"
#endif
#if CONFIG_USE_AUDIO_PROCESSOR
audio_processor_t *afe_audio_processor_create(void);
#endif
#include <model_path.h>
#include <esp_mn_iface.h>
#include <esp_wn_models.h>
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
wake_word_t *afe_wake_word_create(void);
wake_word_t *custom_wake_word_create(void);
#else
wake_word_t *esp_wake_word_create(void);
#endif

#define TAG "AudioService"

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                                    \
    (esp_opus_dec_cfg_t)                                                                                  \
    {                                                                                                     \
        .sample_rate    = (uint32_t)(_sample_rate),                                                       \
        .channel        = ESP_AUDIO_MONO,                                                                 \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms),  \
        .self_delimited = false,                                                                          \
    }

typedef struct as_packet {
    int sample_rate;
    int frame_duration_ms;
    uint32_t timestamp;
    uint8_t *payload;
    size_t payload_size;
} as_packet_t;

typedef struct as_encode_job {
    audio_task_type_t type;
    uint32_t timestamp;
    int16_t *pcm;
    size_t pcm_samples;
} as_encode_job_t;

typedef struct as_play_job {
    uint32_t timestamp;
    int16_t *pcm;
    size_t pcm_samples;
} as_play_job_t;

struct audio_service {
    audio_codec_t *codec;
    audio_callbacks_t callbacks;
    char last_wake_word[128];

    void *opus_enc;
    void *opus_dec;
    SemaphoreHandle_t dec_mtx;

    esp_ae_rate_cvt_handle_t in_resampler;
    esp_ae_rate_cvt_handle_t out_resampler;
    SemaphoreHandle_t in_res_mtx;

    audio_processor_t *processor;
    bool ap_inited;

    wake_word_t *wake;
    bool wake_inited;
    bool wake_is_afe;

#if CONFIG_USE_AUDIO_DEBUGGER
    audio_debugger_t *debugger;
#endif

    srmodel_list_t *models;

    EventGroupHandle_t evg;
    SemaphoreHandle_t q_mtx;
    SemaphoreHandle_t opus_wake;
    SemaphoreHandle_t play_ready;

    as_packet_t *decode_q[MAX_DECODE_PACKETS_IN_QUEUE];
    size_t decode_n;

    as_packet_t *send_q[MAX_SEND_PACKETS_IN_QUEUE];
    size_t send_n;

    as_packet_t *testing_q[AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS];
    size_t testing_n;

    as_encode_job_t *encode_q[MAX_ENCODE_TASKS_IN_QUEUE];
    size_t encode_n;

    as_play_job_t *play_q[MAX_PLAYBACK_TASKS_IN_QUEUE];
    size_t play_n;

    uint32_t ts_ring[MAX_TIMESTAMPS_IN_QUEUE];
    size_t ts_count;

    bool voice_detected;
    bool service_stopped;
    bool audio_input_need_warmup;

    esp_timer_handle_t audio_power_timer;
    int64_t last_input_us;
    int64_t last_output_us;

    int encoder_sample_rate;
    int encoder_duration_ms;
    uint32_t encoder_frame_size;
    uint32_t encoder_outbuf_size;

    int decoder_sample_rate;
    int decoder_duration_ms;
    uint32_t decoder_frame_size;

    TaskHandle_t audio_input_task_handle;
    TaskHandle_t audio_output_task_handle;
    TaskHandle_t opus_codec_task_handle;

};

static audio_service_t *s_instance;

static void packet_free(as_packet_t *p)
{
    if (!p) return;
    free(p->payload);
    free(p);
}

static void encode_job_free(as_encode_job_t *j)
{
    if (!j) return;
    free(j->pcm);
    free(j);
}

static void play_job_free(as_play_job_t *j)
{
    if (!j) return;
    free(j->pcm);
    free(j);
}

static void wake_detected_cb(const char *word, void *ctx);

static void svc_push_encode_job(struct audio_service *s, audio_task_type_t type,
                                int16_t *pcm, size_t pcm_samples, uint32_t ts)
{
    if (!s || !pcm) return;

    for (;;) {
        xSemaphoreTake(s->q_mtx, portMAX_DELAY);
        if (s->encode_n < MAX_ENCODE_TASKS_IN_QUEUE) {
            break;
        }
        xSemaphoreGive(s->q_mtx);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    as_encode_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        xSemaphoreGive(s->q_mtx);
        free(pcm);
        return;
    }
    job->type = type;
    job->pcm = pcm;
    job->pcm_samples = pcm_samples;
    job->timestamp = ts;

    if (type == kAudioTaskTypeEncodeToSendQueue && s->ts_count > 0) {
        job->timestamp = s->ts_ring[0];
        if (s->ts_count > MAX_TIMESTAMPS_IN_QUEUE) {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", (unsigned)s->ts_count);
        }
        memmove(s->ts_ring, s->ts_ring + 1, (s->ts_count - 1) * sizeof(uint32_t));
        s->ts_count--;
    }
    s->encode_q[s->encode_n++] = job;
    xSemaphoreGive(s->q_mtx);
    xSemaphoreGive(s->opus_wake);
}

static void on_processor_out(const int16_t *data, size_t samples, void *ctx)
{
    struct audio_service *s = (struct audio_service *)ctx;
    if (!s || !data || samples == 0) return;
    int16_t *copy = malloc(samples * sizeof(int16_t));
    if (!copy) return;
    memcpy(copy, data, samples * sizeof(int16_t));
    svc_push_encode_job(s, kAudioTaskTypeEncodeToSendQueue, copy, samples, 0);
}

static void on_processor_vad(bool speaking, void *ctx)
{
    struct audio_service *s = (struct audio_service *)ctx;
    if (!s) return;
    s->voice_detected = speaking;
    if (s->callbacks.on_vad_change) {
        s->callbacks.on_vad_change(s->callbacks.user_ctx, speaking);
    }
}

static void wake_detected_cb(const char *word, void *ctx)
{
    struct audio_service *s = (struct audio_service *)ctx;
    if (!s) return;
    if (word) {
        strlcpy(s->last_wake_word, word, sizeof(s->last_wake_word));
    } else {
        s->last_wake_word[0] = '\0';
    }
    if (s->callbacks.on_wake_word) {
        s->callbacks.on_wake_word(s->callbacks.user_ctx, s->last_wake_word);
    }
}


static bool svc_read_audio_data(struct audio_service *s, int16_t *data, size_t *inout_cap_int16,
                                 int sample_rate, int samples_per_ch)
{
    audio_codec_t *codec = s->codec;
    if (!codec || !data || !inout_cap_int16) return false;

    if (!codec->input_enabled) {
        esp_timer_stop(s->audio_power_timer);
        esp_timer_start_periodic(s->audio_power_timer, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        if (codec->ops && codec->ops->enable_input) {
            codec->ops->enable_input(codec, true);
        }
        codec->input_enabled = true;
    }

    size_t need = (size_t)samples_per_ch * (size_t)codec->input_channels;
    if (codec->input_sample_rate != sample_rate) {
        need = (size_t)samples_per_ch * (size_t)codec->input_sample_rate / (size_t)sample_rate * (size_t)codec->input_channels;
    }

    if (need > *inout_cap_int16) {
        return false;
    }

    if (!audio_codec_input_data(codec, data, (int)need)) {
        return false;
    }

    if (codec->input_sample_rate != sample_rate && s->in_resampler) {
        xSemaphoreTake(s->in_res_mtx, portMAX_DELAY);
        uint32_t in_sample_num = (uint32_t)(need / (size_t)codec->input_channels);
        uint32_t output_samples = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(s->in_resampler, in_sample_num, &output_samples);
        size_t out_need = (size_t)output_samples * (size_t)codec->input_channels;
        int16_t *resampled = malloc(out_need * sizeof(int16_t));
        if (!resampled) {
            xSemaphoreGive(s->in_res_mtx);
            return false;
        }
        uint32_t actual_output = output_samples;
        esp_ae_rate_cvt_process(s->in_resampler, (esp_ae_sample_t)data, in_sample_num,
                                (esp_ae_sample_t)resampled, &actual_output);
        size_t out_count = (size_t)actual_output * (size_t)codec->input_channels;
        if (out_count > *inout_cap_int16) {
            free(resampled);
            xSemaphoreGive(s->in_res_mtx);
            return false;
        }
        memcpy(data, resampled, out_count * sizeof(int16_t));
        free(resampled);
        need = out_count;
        xSemaphoreGive(s->in_res_mtx);
    }

    s->last_input_us = esp_timer_get_time();
    *inout_cap_int16 = need;
    return true;
}

static void audio_power_timer_cb(void *arg)
{
    struct audio_service *s = (struct audio_service *)arg;
    if (!s || !s->codec) return;
    int64_t now = esp_timer_get_time();
    int64_t in_dt = (now - s->last_input_us) / 1000;
    int64_t out_dt = (now - s->last_output_us) / 1000;
    audio_codec_t *c = s->codec;

    if (in_dt > AUDIO_POWER_TIMEOUT_MS && c->input_enabled) {
        if (c->ops && c->ops->enable_input) {
            c->ops->enable_input(c, false);
        }
        c->input_enabled = false;
    }
    if (out_dt > AUDIO_POWER_TIMEOUT_MS && c->output_enabled) {
        if (!(c->duplex && c->input_enabled)) {
            if (c->ops && c->ops->enable_output) {
                c->ops->enable_output(c, false);
            }
            c->output_enabled = false;
        }
    }
    if (!c->input_enabled && !c->output_enabled) {
        esp_timer_stop(s->audio_power_timer);
    }
}

static void audio_input_task(void *arg)
{
    struct audio_service *s = (struct audio_service *)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s->evg,
                                               AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                                   AS_EVENT_AUDIO_PROCESSOR_RUNNING,
                                               pdFALSE, pdFALSE, portMAX_DELAY);
        if (s->service_stopped) {
            break;
        }
        if (s->audio_input_need_warmup) {
            s->audio_input_need_warmup = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (s->testing_n >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                /* Caller should disable testing; mirror C++ log only */
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            size_t cap = (size_t)samples * (size_t)(s->codec ? s->codec->input_channels : 1) * 2;
            int16_t *buf = malloc(cap * sizeof(int16_t));
            if (!buf) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (svc_read_audio_data(s, buf, &cap, 16000, samples)) {
                size_t mono_n = cap;
                if (s->codec->input_channels == 2) {
                    mono_n = cap / 2;
                    for (size_t i = 0; i < mono_n; ++i) {
                        buf[i] = buf[i * 2];
                    }
                }
                int16_t *copy = malloc(mono_n * sizeof(int16_t));
                if (copy) {
                    memcpy(copy, buf, mono_n * sizeof(int16_t));
                    svc_push_encode_job(s, kAudioTaskTypeEncodeToTestingQueue, copy, mono_n, 0);
                }
                free(buf);
                continue;
            }
            free(buf);
        }

        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160;
            size_t cap = (size_t)samples * (size_t)(s->codec ? s->codec->input_channels : 1) * 2;
            int16_t *buf = malloc(cap * sizeof(int16_t));
            if (!buf) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (svc_read_audio_data(s, buf, &cap, 16000, samples)) {
                if (bits & AS_EVENT_WAKE_WORD_RUNNING && s->wake) {
                    wake_word_feed(s->wake, buf, cap);
                }
                if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING && s->processor) {
                    audio_processor_feed(s->processor, buf, cap);
                }
                free(buf);
                continue;
            }
            free(buf);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "Audio input task stopped");
    vTaskDelete(NULL);
}

static void audio_output_task(void *arg)
{
    struct audio_service *s = (struct audio_service *)arg;
    for (;;) {
        if (xSemaphoreTake(s->play_ready, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s->service_stopped) {
            break;
        }
        xSemaphoreTake(s->q_mtx, portMAX_DELAY);
        if (s->play_n == 0) {
            xSemaphoreGive(s->q_mtx);
            continue;
        }
        as_play_job_t *job = s->play_q[0];
        memmove(s->play_q, s->play_q + 1, (s->play_n - 1) * sizeof(s->play_q[0]));
        s->play_n--;
        xSemaphoreGive(s->q_mtx);

        audio_codec_t *codec = s->codec;
        if (!codec->output_enabled) {
            esp_timer_stop(s->audio_power_timer);
            esp_timer_start_periodic(s->audio_power_timer, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            if (codec->ops && codec->ops->enable_output) {
                codec->ops->enable_output(codec, true);
            }
            codec->output_enabled = true;
        }

        if (codec->ops && codec->ops->write) {
            codec->ops->write(codec, job->pcm, (int)(job->pcm_samples));
        }

        s->last_output_us = esp_timer_get_time();

#if CONFIG_USE_SERVER_AEC
        if (job->timestamp > 0) {
            xSemaphoreTake(s->q_mtx, portMAX_DELAY);
            if (s->ts_count < MAX_TIMESTAMPS_IN_QUEUE) {
                s->ts_ring[s->ts_count++] = job->timestamp;
            }
            xSemaphoreGive(s->q_mtx);
        }
#endif
        play_job_free(job);
        xSemaphoreGive(s->opus_wake);
    }
    ESP_LOGW(TAG, "Audio output task stopped");
    vTaskDelete(NULL);
}


static void svc_set_decode_sample_rate(struct audio_service *s, int sample_rate, int frame_duration_ms)
{
    if (s->decoder_sample_rate == sample_rate && s->decoder_duration_ms == frame_duration_ms) {
        return;
    }
    xSemaphoreTake(s->dec_mtx, portMAX_DELAY);
    if (s->opus_dec) {
        esp_opus_dec_close(s->opus_dec);
        s->opus_dec = NULL;
    }
    xSemaphoreGive(s->dec_mtx);

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration_ms);
    void *dec = NULL;
    esp_err_t ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(opus_dec_cfg), &dec);
    if (dec == NULL) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return;
    }
    xSemaphoreTake(s->dec_mtx, portMAX_DELAY);
    s->opus_dec = dec;
    xSemaphoreGive(s->dec_mtx);

    s->decoder_sample_rate = sample_rate;
    s->decoder_duration_ms = frame_duration_ms;
    s->decoder_frame_size = (uint32_t)sample_rate / 1000U * (uint32_t)frame_duration_ms;

    audio_codec_t *codec = s->codec;
    if (!codec) {
        return;
    }
    if (s->decoder_sample_rate != codec->output_sample_rate) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", s->decoder_sample_rate, codec->output_sample_rate);
        if (s->out_resampler) {
            esp_ae_rate_cvt_close(s->out_resampler);
            s->out_resampler = NULL;
        }
        esp_ae_rate_cvt_cfg_t cfg = RATE_CVT_CFG(s->decoder_sample_rate, codec->output_sample_rate, ESP_AUDIO_MONO);
        esp_err_t rr = esp_ae_rate_cvt_open(&cfg, &s->out_resampler);
        if (s->out_resampler == NULL) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", rr);
        }
    } else if (s->out_resampler) {
        esp_ae_rate_cvt_close(s->out_resampler);
        s->out_resampler = NULL;
    }
}

static bool svc_push_decode(struct audio_service *s, as_packet_t *pkt, bool wait)
{
    for (;;) {
        xSemaphoreTake(s->q_mtx, portMAX_DELAY);
        if (s->decode_n < MAX_DECODE_PACKETS_IN_QUEUE) {
            s->decode_q[s->decode_n++] = pkt;
            xSemaphoreGive(s->q_mtx);
            xSemaphoreGive(s->opus_wake);
            return true;
        }
        xSemaphoreGive(s->q_mtx);
        if (!wait) {
            packet_free(pkt);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void ogg_page_cb(const uint8_t *data, int sample_rate, size_t len, void *ctx)
{
    struct audio_service *s = (struct audio_service *)ctx;
    as_packet_t *pkt = calloc(1, sizeof(*pkt));
    if (!pkt) return;
    pkt->sample_rate = sample_rate;
    pkt->frame_duration_ms = 60;
    pkt->timestamp = 0;
    pkt->payload = malloc(len);
    if (!pkt->payload) {
        free(pkt);
        return;
    }
    memcpy(pkt->payload, data, len);
    pkt->payload_size = len;
    svc_push_decode(s, pkt, true);
}

static void opus_codec_task(void *arg)
{
    struct audio_service *s = (struct audio_service *)arg;
    for (;;) {
        xSemaphoreTake(s->opus_wake, portMAX_DELAY);
        if (s->service_stopped) {
            break;
        }
        bool progress = true;
        while (progress) {
            progress = false;

            /* Decode */
            xSemaphoreTake(s->q_mtx, portMAX_DELAY);
            bool can_dec = (s->decode_n > 0) && (s->play_n < MAX_PLAYBACK_TASKS_IN_QUEUE);
            as_packet_t *pkt = NULL;
            if (can_dec) {
                pkt = s->decode_q[0];
                memmove(s->decode_q, s->decode_q + 1, (s->decode_n - 1) * sizeof(s->decode_q[0]));
                s->decode_n--;
            }
            xSemaphoreGive(s->q_mtx);
            if (pkt) {
                svc_set_decode_sample_rate(s, pkt->sample_rate, pkt->frame_duration_ms);

                as_play_job_t *pj = calloc(1, sizeof(*pj));
                if (pj && s->opus_dec) {
                    pj->timestamp = pkt->timestamp;
                    pj->pcm = malloc(s->decoder_frame_size * sizeof(int16_t));
                    if (pj->pcm) {
                        esp_audio_dec_in_raw_t raw = {
                            .buffer = pkt->payload,
                            .len = (uint32_t)pkt->payload_size,
                            .consumed = 0,
                            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                        };
                        esp_audio_dec_out_frame_t out_frame = {
                            .buffer = (uint8_t *)pj->pcm,
                            .len = (uint32_t)(s->decoder_frame_size * sizeof(int16_t)),
                            .decoded_size = 0,
                        };
                        esp_audio_dec_info_t dec_info = {};
                        xSemaphoreTake(s->dec_mtx, portMAX_DELAY);
                        esp_err_t dret = esp_opus_dec_decode(s->opus_dec, &raw, &out_frame, &dec_info);
                        xSemaphoreGive(s->dec_mtx);
                        if (dret == ESP_AUDIO_ERR_OK) {
                            pj->pcm_samples = out_frame.decoded_size / sizeof(int16_t);
                            if (s->decoder_sample_rate != s->codec->output_sample_rate && s->out_resampler) {
                                uint32_t target_size = 0;
                                esp_ae_rate_cvt_get_max_out_sample_num(s->out_resampler, (uint32_t)pj->pcm_samples,
                                                                       &target_size);
                                int16_t *rs = malloc((size_t)target_size * sizeof(int16_t));
                                if (rs) {
                                    uint32_t ao = target_size;
                                    esp_ae_rate_cvt_process(s->out_resampler, (esp_ae_sample_t)pj->pcm,
                                                            (uint32_t)pj->pcm_samples, (esp_ae_sample_t)rs, &ao);
                                    free(pj->pcm);
                                    pj->pcm = rs;
                                    pj->pcm_samples = ao;
                                }
                            }
                            xSemaphoreTake(s->q_mtx, portMAX_DELAY);
                            if (s->play_n < MAX_PLAYBACK_TASKS_IN_QUEUE) {
                                s->play_q[s->play_n++] = pj;
                                xSemaphoreGive(s->q_mtx);
                                xSemaphoreGive(s->play_ready);
                            } else {
                                xSemaphoreGive(s->q_mtx);
                                play_job_free(pj);
                            }
                        } else {
                            ESP_LOGE(TAG, "Failed to decode audio, error code: %d", dret);
                            play_job_free(pj);
                        }
                    } else {
                        free(pj);
                    }
                } else if (pj) {
                    free(pj);
                }
                packet_free(pkt);
                progress = true;
                xSemaphoreGive(s->opus_wake);
                continue;
            }

            /* Encode */
            xSemaphoreTake(s->q_mtx, portMAX_DELAY);
            bool can_enc = (s->encode_n > 0) && (s->send_n < MAX_SEND_PACKETS_IN_QUEUE);
            as_encode_job_t *job = NULL;
            if (can_enc) {
                job = s->encode_q[0];
                memmove(s->encode_q, s->encode_q + 1, (s->encode_n - 1) * sizeof(s->encode_q[0]));
                s->encode_n--;
            }
            xSemaphoreGive(s->q_mtx);
            if (!job) {
                progress = false;
                break;
            }

            if (s->opus_enc && job->pcm_samples == s->encoder_frame_size) {
                uint8_t *buf = malloc(s->encoder_outbuf_size);
                if (buf) {
                    esp_audio_enc_in_frame_t in = {
                        .buffer = (uint8_t *)job->pcm,
                        .len = (uint32_t)(s->encoder_frame_size * sizeof(int16_t)),
                    };
                    esp_audio_enc_out_frame_t out = {
                        .buffer = buf,
                        .len = (uint32_t)s->encoder_outbuf_size,
                        .encoded_bytes = 0,
                    };
                    esp_err_t eret = esp_opus_enc_process(s->opus_enc, &in, &out);
                    if (eret == ESP_AUDIO_ERR_OK) {
                        as_packet_t *outp = calloc(1, sizeof(*outp));
                        if (outp) {
                            outp->frame_duration_ms = OPUS_FRAME_DURATION_MS;
                            outp->sample_rate = 16000;
                            outp->timestamp = job->timestamp;
                            outp->payload = malloc(out.encoded_bytes);
                            if (outp->payload) {
                                memcpy(outp->payload, buf, out.encoded_bytes);
                                outp->payload_size = out.encoded_bytes;
                                if (job->type == kAudioTaskTypeEncodeToSendQueue) {
                                    xSemaphoreTake(s->q_mtx, portMAX_DELAY);
                                    if (s->send_n < MAX_SEND_PACKETS_IN_QUEUE) {
                                        s->send_q[s->send_n++] = outp;
                                        xSemaphoreGive(s->q_mtx);
                                        if (s->callbacks.on_send_queue_available) {
                                            s->callbacks.on_send_queue_available(s->callbacks.user_ctx);
                                        }
                                    } else {
                                        xSemaphoreGive(s->q_mtx);
                                        packet_free(outp);
                                    }
                                } else if (job->type == kAudioTaskTypeEncodeToTestingQueue) {
                                    xSemaphoreTake(s->q_mtx, portMAX_DELAY);
                                    if (s->testing_n < sizeof(s->testing_q) / sizeof(s->testing_q[0])) {
                                        s->testing_q[s->testing_n++] = outp;
                                    } else {
                                        packet_free(outp);
                                    }
                                    xSemaphoreGive(s->q_mtx);
                                }
                            } else {
                                free(outp);
                            }
                        }
                    } else {
                        ESP_LOGE(TAG, "Failed to encode audio, error code: %d", eret);
                    }
                    free(buf);
                }
            } else {
                ESP_LOGE(TAG, "Encoder not ready or bad frame size (got %u, expected %u)",
                         (unsigned)job->pcm_samples, (unsigned)s->encoder_frame_size);
            }
            encode_job_free(job);
            progress = true;
            xSemaphoreGive(s->opus_wake);
        }
    }
    ESP_LOGW(TAG, "Opus codec task stopped");
    vTaskDelete(NULL);
}


static void svc_flush_queues(struct audio_service *s)
{
    xSemaphoreTake(s->q_mtx, portMAX_DELAY);
    for (size_t i = 0; i < s->decode_n; ++i) {
        packet_free(s->decode_q[i]);
        s->decode_q[i] = NULL;
    }
    s->decode_n = 0;
    for (size_t i = 0; i < s->send_n; ++i) {
        packet_free(s->send_q[i]);
        s->send_q[i] = NULL;
    }
    s->send_n = 0;
    for (size_t i = 0; i < s->testing_n; ++i) {
        packet_free(s->testing_q[i]);
        s->testing_q[i] = NULL;
    }
    s->testing_n = 0;
    for (size_t i = 0; i < s->encode_n; ++i) {
        encode_job_free(s->encode_q[i]);
        s->encode_q[i] = NULL;
    }
    s->encode_n = 0;
    for (size_t i = 0; i < s->play_n; ++i) {
        play_job_free(s->play_q[i]);
        s->play_q[i] = NULL;
    }
    s->play_n = 0;
    s->ts_count = 0;
    xSemaphoreGive(s->q_mtx);
}

audio_service_t *audio_service_get_instance(void)
{
    return s_instance;
}

audio_service_t *audio_service_create(const audio_service_cfg_t *cfg)
{
    (void)cfg;
    struct audio_service *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->evg = xEventGroupCreate();
    s->q_mtx = xSemaphoreCreateMutex();
    s->opus_wake = xSemaphoreCreateCounting(32, 0);
    s->play_ready = xSemaphoreCreateBinary();
    s->dec_mtx = xSemaphoreCreateMutex();
    s->in_res_mtx = xSemaphoreCreateMutex();
    if (!s->evg || !s->q_mtx || !s->opus_wake || !s->play_ready || !s->dec_mtx || !s->in_res_mtx) {
        /* Leak partial on failure — rare */
        return NULL;
    }
    s->service_stopped = true;
    s->last_wake_word[0] = '\0';
    s_instance = (audio_service_t *)s;
    return (audio_service_t *)s;
}

void audio_service_destroy(audio_service_t *svc)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s) return;
    audio_service_stop(svc);
    if (s->wake) {
        wake_word_destroy(s->wake);
        s->wake = NULL;
    }
    if (s->processor) {
        audio_processor_destroy(s->processor);
        s->processor = NULL;
    }
#if CONFIG_USE_AUDIO_DEBUGGER
    if (s->debugger) {
        audio_debugger_destroy(s->debugger);
        s->debugger = NULL;
    }
#endif
    if (s->opus_enc) {
        esp_opus_enc_close(s->opus_enc);
        s->opus_enc = NULL;
    }
    if (s->opus_dec) {
        esp_opus_dec_close(s->opus_dec);
        s->opus_dec = NULL;
    }
    if (s->in_resampler) {
        esp_ae_rate_cvt_close(s->in_resampler);
    }
    if (s->out_resampler) {
        esp_ae_rate_cvt_close(s->out_resampler);
    }
    svc_flush_queues(s);
    vSemaphoreDelete(s->opus_wake);
    vSemaphoreDelete(s->play_ready);
    vSemaphoreDelete(s->q_mtx);
    vSemaphoreDelete(s->dec_mtx);
    vSemaphoreDelete(s->in_res_mtx);
    vEventGroupDelete(s->evg);
    if (s->audio_power_timer) {
        esp_timer_delete(s->audio_power_timer);
    }
    free(s);
    if (s_instance == svc) {
        s_instance = NULL;
    }
}

esp_err_t audio_service_init(audio_service_t *svc, void *audio_codec_handle)
{
    struct audio_service *s = (struct audio_service *)svc;
    audio_codec_t *codec = (audio_codec_t *)audio_codec_handle;
    if (!s || !codec) {
        return ESP_ERR_INVALID_ARG;
    }
    s->codec = codec;
    audio_codec_base_start(codec);
    if (codec->ops && codec->ops->start && codec->ops->start != audio_codec_base_start) {
        codec->ops->start(codec);
    }

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(codec->output_sample_rate, OPUS_FRAME_DURATION_MS);
    esp_err_t ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(opus_dec_cfg), &s->opus_dec);
    if (s->opus_dec == NULL) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        s->decoder_sample_rate = codec->output_sample_rate;
        s->decoder_duration_ms = OPUS_FRAME_DURATION_MS;
        s->decoder_frame_size = (uint32_t)s->decoder_sample_rate / 1000U * (uint32_t)OPUS_FRAME_DURATION_MS;
    }

    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(opus_enc_cfg), &s->opus_enc);
    if (s->opus_enc == NULL) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        s->encoder_sample_rate = 16000;
        s->encoder_duration_ms = OPUS_FRAME_DURATION_MS;
        int enc_in_bytes = 0;
        int enc_out_bytes = 0;
        esp_opus_enc_get_frame_size(s->opus_enc, &enc_in_bytes, &enc_out_bytes);
        s->encoder_frame_size = (uint32_t)enc_in_bytes / (uint32_t)sizeof(int16_t);
        s->encoder_outbuf_size = (uint32_t)enc_out_bytes;
    }

    if (codec->input_sample_rate != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg =
            RATE_CVT_CFG(codec->input_sample_rate, ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels);
        esp_err_t rr = esp_ae_rate_cvt_open(&input_resampler_cfg, &s->in_resampler);
        if (s->in_resampler == NULL) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", rr);
        }
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    s->processor = afe_audio_processor_create();
#else
    s->processor = no_audio_processor_create();
#endif
    if (!s->processor) {
        ESP_LOGE(TAG, "Failed to create audio processor");
        return ESP_FAIL;
    }
    audio_processor_set_output_cb(s->processor, on_processor_out, s);
    audio_processor_set_vad_cb(s->processor, on_processor_vad, s);

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = &audio_power_timer_cb,
        .arg = s,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &s->audio_power_timer);

    int64_t t0 = esp_timer_get_time();
    s->last_input_us = t0;
    s->last_output_us = t0;
    return ESP_OK;
}

esp_err_t audio_service_start(audio_service_t *svc)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    s->service_stopped = false;
    xEventGroupClearBits(s->evg, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    esp_timer_start_periodic(s->audio_power_timer, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    xTaskCreatePinnedToCore(audio_input_task, "audio_input", 2048 * 3, s, 8, &s->audio_input_task_handle, 0);
    xTaskCreate(audio_output_task, "audio_output", 2048 * 2, s, 4, &s->audio_output_task_handle);
#else
    xTaskCreate(audio_input_task, "audio_input", 2048 * 2, s, 8, &s->audio_input_task_handle);
    xTaskCreate(audio_output_task, "audio_output", 2048, s, 4, &s->audio_output_task_handle);
#endif
    xTaskCreate(opus_codec_task, "opus_codec", 2048 * 12, s, 2, &s->opus_codec_task_handle);
    return ESP_OK;
}

esp_err_t audio_service_stop(audio_service_t *svc)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_timer_stop(s->audio_power_timer);
    s->service_stopped = true;
    xEventGroupSetBits(s->evg, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    xSemaphoreGive(s->opus_wake);
    xSemaphoreGive(s->play_ready);
    svc_flush_queues(s);
    return ESP_OK;
}

esp_err_t audio_service_set_callbacks(audio_service_t *svc, const audio_callbacks_t *callbacks)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s || !callbacks) {
        return ESP_ERR_INVALID_ARG;
    }
    s->callbacks = *callbacks;
    return ESP_OK;
}

esp_err_t audio_service_enable_wake_word(audio_service_t *svc, bool enable)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s || !s->wake) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable) {
        if (!s->wake_inited) {
            if (!wake_word_initialize(s->wake, s->codec, s->models)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return ESP_FAIL;
            }
            s->wake_inited = true;
        }
        if (s->in_resampler) {
            xSemaphoreTake(s->in_res_mtx, portMAX_DELAY);
            esp_ae_rate_cvt_reset(s->in_resampler);
            xSemaphoreGive(s->in_res_mtx);
        }
        wake_word_start(s->wake);
        xEventGroupSetBits(s->evg, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_stop(s->wake);
        xEventGroupClearBits(s->evg, AS_EVENT_WAKE_WORD_RUNNING);
    }
    return ESP_OK;
}

esp_err_t audio_service_enable_voice_processing(audio_service_t *svc, bool enable)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s || !s->processor) {
        return ESP_ERR_INVALID_ARG;
    }
    if (enable) {
        if (!s->ap_inited) {
            audio_processor_initialize(s->processor, s->codec, OPUS_FRAME_DURATION_MS, s->models);
            s->ap_inited = true;
        }
        audio_service_reset_decoder(svc);
        s->audio_input_need_warmup = true;
        if (s->in_resampler) {
            xSemaphoreTake(s->in_res_mtx, portMAX_DELAY);
            esp_ae_rate_cvt_reset(s->in_resampler);
            xSemaphoreGive(s->in_res_mtx);
        }
        audio_processor_start(s->processor);
        xEventGroupSetBits(s->evg, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        audio_processor_stop(s->processor);
        xEventGroupClearBits(s->evg, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
    return ESP_OK;
}

esp_err_t audio_service_enable_audio_testing(audio_service_t *svc, bool enable)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    if (enable) {
        xEventGroupSetBits(s->evg, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(s->evg, AS_EVENT_AUDIO_TESTING_RUNNING);
        xSemaphoreTake(s->q_mtx, portMAX_DELAY);
        while (s->testing_n > 0) {
            as_packet_t *p = s->testing_q[0];
            memmove(s->testing_q, s->testing_q + 1, (s->testing_n - 1) * sizeof(s->testing_q[0]));
            s->testing_n--;
            if (s->decode_n < MAX_DECODE_PACKETS_IN_QUEUE) {
                s->decode_q[s->decode_n++] = p;
            } else {
                packet_free(p);
            }
        }
        xSemaphoreGive(s->q_mtx);
        xSemaphoreGive(s->opus_wake);
    }
    return ESP_OK;
}

bool audio_service_is_idle(audio_service_t *svc)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s) return true;
    xSemaphoreTake(s->q_mtx, portMAX_DELAY);
    bool idle = (s->encode_n == 0 && s->decode_n == 0 && s->play_n == 0 && s->testing_n == 0);
    xSemaphoreGive(s->q_mtx);
    return idle;
}

bool audio_service_is_voice_detected(const audio_service_t *svc)
{
    const struct audio_service *s = (const struct audio_service *)svc;
    return s && s->voice_detected;
}

const char *audio_service_get_last_wake_word(audio_service_t *svc)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s) {
        return "";
    }
    return s->last_wake_word;
}

esp_err_t audio_service_push_decode_packet(audio_service_t *svc, const audio_packet_t *packet)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s || !packet || !packet->payload) {
        return ESP_ERR_INVALID_ARG;
    }
    as_packet_t *p = calloc(1, sizeof(*p));
    if (!p) {
        return ESP_ERR_NO_MEM;
    }
    p->sample_rate = packet->sample_rate;
    p->frame_duration_ms = packet->frame_duration_ms;
    p->timestamp = packet->timestamp;
    p->payload = malloc(packet->payload_size);
    if (!p->payload) {
        free(p);
        return ESP_ERR_NO_MEM;
    }
    memcpy(p->payload, packet->payload, packet->payload_size);
    p->payload_size = packet->payload_size;
    return svc_push_decode(s, p, false) ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_service_pop_send_packet(audio_service_t *svc, audio_packet_t *out_packet)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s || !out_packet) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s->q_mtx, portMAX_DELAY);
    if (s->send_n == 0) {
        xSemaphoreGive(s->q_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    as_packet_t *p = s->send_q[0];
    memmove(s->send_q, s->send_q + 1, (s->send_n - 1) * sizeof(s->send_q[0]));
    s->send_n--;
    xSemaphoreGive(s->q_mtx);
    xSemaphoreGive(s->opus_wake);

    out_packet->sample_rate = p->sample_rate;
    out_packet->frame_duration_ms = p->frame_duration_ms;
    out_packet->timestamp = p->timestamp;
    out_packet->payload_size = p->payload_size;
    out_packet->payload = (uint8_t *)malloc(p->payload_size);
    if (!out_packet->payload) {
        packet_free(p);
        return ESP_ERR_NO_MEM;
    }
    memcpy(out_packet->payload, p->payload, p->payload_size);
    packet_free(p);
    return ESP_OK;
}

esp_err_t audio_service_pop_wake_word_packet(audio_service_t *svc, audio_packet_t *out_packet)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s || !out_packet || !s->wake) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[4096];
    size_t out_len = 0;
    if (!wake_word_get_opus(s->wake, buf, sizeof(buf), &out_len)) {
        return ESP_ERR_NOT_FOUND;
    }
    out_packet->sample_rate = 0;
    out_packet->frame_duration_ms = OPUS_FRAME_DURATION_MS;
    out_packet->timestamp = 0;
    out_packet->payload_size = out_len;
    out_packet->payload = (uint8_t *)malloc(out_len);
    if (!out_packet->payload) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(out_packet->payload, buf, out_len);
    return ESP_OK;
}

void audio_service_free_packet(audio_packet_t *packet)
{
    if (!packet) return;
    free(packet->payload);
    packet->payload = NULL;
    packet->payload_size = 0;
}

esp_err_t audio_service_play_ogg(audio_service_t *svc, const uint8_t *data, size_t len)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_codec_t *codec = s->codec;
    if (!codec->output_enabled) {
        esp_timer_stop(s->audio_power_timer);
        esp_timer_start_periodic(s->audio_power_timer, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        if (codec->ops && codec->ops->enable_output) {
            codec->ops->enable_output(codec, true);
        }
        codec->output_enabled = true;
    }
    ogg_demuxer_t *d = ogg_demuxer_create();
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    ogg_demuxer_set_callback(d, ogg_page_cb, s);
    ogg_demuxer_reset(d);
    ogg_demuxer_process(d, data, len);
    ogg_demuxer_destroy(d);
    return ESP_OK;
}

esp_err_t audio_service_reset_decoder(audio_service_t *svc)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s->q_mtx, portMAX_DELAY);
    xSemaphoreTake(s->dec_mtx, portMAX_DELAY);
    if (s->opus_dec) {
        esp_opus_dec_reset(s->opus_dec);
    }
    xSemaphoreGive(s->dec_mtx);
    for (size_t i = 0; i < s->decode_n; ++i) {
        packet_free(s->decode_q[i]);
    }
    s->decode_n = 0;
    for (size_t i = 0; i < s->play_n; ++i) {
        play_job_free(s->play_q[i]);
    }
    s->play_n = 0;
    for (size_t i = 0; i < s->testing_n; ++i) {
        packet_free(s->testing_q[i]);
    }
    s->testing_n = 0;
    s->ts_count = 0;
    xSemaphoreGive(s->q_mtx);
    xSemaphoreGive(s->opus_wake);
    return ESP_OK;
}

esp_err_t audio_service_encode_wake_word(audio_service_t *svc)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s || !s->wake) {
        return ESP_ERR_INVALID_ARG;
    }
    wake_word_encode_data(s->wake);
    return ESP_OK;
}

esp_err_t audio_service_enable_device_aec(audio_service_t *svc, bool enable)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s || !s->processor) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s->ap_inited) {
        audio_processor_initialize(s->processor, s->codec, OPUS_FRAME_DURATION_MS, s->models);
        s->ap_inited = true;
    }
    audio_processor_enable_device_aec(s->processor, enable);
    return ESP_OK;
}

void audio_service_wait_for_playback_empty(audio_service_t *svc)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s) return;
    while (!s->service_stopped) {
        xSemaphoreTake(s->q_mtx, portMAX_DELAY);
        bool empty = (s->decode_n == 0 && s->play_n == 0);
        xSemaphoreGive(s->q_mtx);
        if (empty) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

bool audio_service_is_wake_word_running(const audio_service_t *svc)
{
    const struct audio_service *s = (const struct audio_service *)svc;
    return s && (xEventGroupGetBits(s->evg) & AS_EVENT_WAKE_WORD_RUNNING);
}

bool audio_service_is_audio_processor_running(const audio_service_t *svc)
{
    const struct audio_service *s = (const struct audio_service *)svc;
    return s && (xEventGroupGetBits(s->evg) & AS_EVENT_AUDIO_PROCESSOR_RUNNING);
}

bool audio_service_is_afe_wake_word(audio_service_t *svc)
{
    struct audio_service *s = (struct audio_service *)svc;
    return s && s->wake_is_afe;
}

esp_err_t audio_service_set_models_list(audio_service_t *svc, void *models)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    s->models = (srmodel_list_t *)models;
    if (s->wake) {
        wake_word_destroy(s->wake);
        s->wake = NULL;
    }
    s->wake_inited = false;
    s->wake_is_afe = false;
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (s->models && esp_srmodel_filter(s->models, ESP_MN_PREFIX, NULL) != NULL) {
        s->wake = custom_wake_word_create();
    } else if (s->models && esp_srmodel_filter(s->models, ESP_WN_PREFIX, NULL) != NULL) {
        s->wake = afe_wake_word_create();
        s->wake_is_afe = true;
    }
#else
    if (s->models && esp_srmodel_filter(s->models, ESP_WN_PREFIX, NULL) != NULL) {
        s->wake = esp_wake_word_create();
    }
#endif
    if (s->wake) {
        wake_word_set_detected_cb(s->wake, wake_detected_cb, s);
    }
    return ESP_OK;
}

size_t audio_service_read_audio_data(audio_service_t *svc, int16_t *out, size_t out_cap_int16, int sample_rate,
                                     int samples_per_ch)
{
    struct audio_service *s = (struct audio_service *)svc;
    if (!s || !out || out_cap_int16 == 0) {
        return 0;
    }
    size_t cap = out_cap_int16;
    if (!svc_read_audio_data(s, out, &cap, sample_rate, samples_per_ch)) {
        return 0;
    }
    return cap;
}

