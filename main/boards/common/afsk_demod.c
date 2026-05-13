#include "afsk_demod.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "display.h"
#include "device_state.h"
#include "c_api/app_c_api.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "AUDIO_WIFI_CONFIG";

/* ============================================================
 * Ring buffer (float) - sliding window for DSP
 * ============================================================ */

typedef struct {
    float *data;
    size_t capacity;
    size_t count;
    size_t head;
} ringbuf_f_t;

static void rbf_init(ringbuf_f_t *rb, size_t cap) {
    rb->data = (float *)calloc(cap, sizeof(float));
    rb->capacity = cap;
    rb->count = 0;
    rb->head = 0;
}

static void rbf_free(ringbuf_f_t *rb) { free(rb->data); }
static void rbf_clear(ringbuf_f_t *rb) { rb->count = 0; rb->head = 0; }

static void rbf_push(ringbuf_f_t *rb, float val) {
    size_t idx = (rb->head + rb->count) % rb->capacity;
    rb->data[idx] = val;
    if (rb->count < rb->capacity) {
        rb->count++;
    } else {
        rb->head = (rb->head + 1) % rb->capacity;
    }
}

static void rbf_pop_front(ringbuf_f_t *rb) {
    if (rb->count > 0) {
        rb->head = (rb->head + 1) % rb->capacity;
        rb->count--;
    }
}

static float rbf_at(const ringbuf_f_t *rb, size_t i) {
    return rb->data[(rb->head + i) % rb->capacity];
}

/* ============================================================
 * Ring buffer (uint8_t) - pattern matching window
 * ============================================================ */

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t count;
    size_t head;
} ringbuf_u8_t;

static void rbu8_init(ringbuf_u8_t *rb, size_t cap) {
    rb->data = (uint8_t *)calloc(cap, sizeof(uint8_t));
    rb->capacity = cap;
    rb->count = 0;
    rb->head = 0;
}

static void rbu8_free(ringbuf_u8_t *rb) { free(rb->data); }
static void rbu8_clear(ringbuf_u8_t *rb) { rb->count = 0; rb->head = 0; }

static void rbu8_push(ringbuf_u8_t *rb, uint8_t val) {
    size_t idx = (rb->head + rb->count) % rb->capacity;
    rb->data[idx] = val;
    if (rb->count < rb->capacity) {
        rb->count++;
    } else {
        rb->head = (rb->head + 1) % rb->capacity;
    }
}

static uint8_t rbu8_at(const ringbuf_u8_t *rb, size_t i) {
    return rb->data[(rb->head + i) % rb->capacity];
}

static bool rbu8_equals(const ringbuf_u8_t *rb, const uint8_t *pattern, size_t len) {
    if (rb->count != len) return false;
    for (size_t i = 0; i < len; i++) {
        if (rbu8_at(rb, i) != pattern[i]) return false;
    }
    return true;
}

/* ============================================================
 * FrequencyDetector - Goertzel algorithm for single-frequency
 * detection in AFSK demodulation
 * ============================================================ */

struct afsk_freq_detector {
    float frequency;
    size_t window_size;
    float frequency_bin;
    float angular_frequency;
    float cos_coeff;
    float sin_coeff;
    float filter_coeff;
    float state[2]; /* [0]=S[-2], [1]=S[-1] */
};

afsk_freq_detector_t *afsk_freq_detector_create(float frequency, size_t window_size) {
    afsk_freq_detector_t *det = (afsk_freq_detector_t *)calloc(1, sizeof(*det));
    if (!det) return NULL;
    det->frequency = frequency;
    det->window_size = window_size;
    det->frequency_bin = floorf(frequency * (float)window_size);
    det->angular_frequency = 2.0f * (float)M_PI * frequency;
    det->cos_coeff = cosf(det->angular_frequency);
    det->sin_coeff = sinf(det->angular_frequency);
    det->filter_coeff = 2.0f * det->cos_coeff;
    det->state[0] = 0.0f;
    det->state[1] = 0.0f;
    return det;
}

void afsk_freq_detector_destroy(afsk_freq_detector_t *det) {
    free(det);
}

void afsk_freq_detector_reset(afsk_freq_detector_t *det) {
    if (!det) return;
    det->state[0] = 0.0f;
    det->state[1] = 0.0f;
}

void afsk_freq_detector_process_sample(afsk_freq_detector_t *det, float sample) {
    if (!det) return;
    float s2 = det->state[0];
    float s1 = det->state[1];
    float s0 = sample + det->filter_coeff * s1 - s2;
    det->state[0] = s1;
    det->state[1] = s0;
}

float afsk_freq_detector_get_amplitude(const afsk_freq_detector_t *det) {
    if (!det) return 0.0f;
    float s1 = det->state[1];
    float s2 = det->state[0];
    float re = det->cos_coeff * s1 - s2;
    float im = det->sin_coeff * s1;
    return sqrtf(re * re + im * im) / ((float)det->window_size / 2.0f);
}

/* ============================================================
 * AudioSignalProcessor - Mark/Space frequency pair detector
 * ============================================================ */

struct afsk_signal_processor {
    ringbuf_f_t input_buffer;
    size_t input_buffer_size;
    size_t output_sample_count;
    size_t samples_per_bit;
    afsk_freq_detector_t *mark_det;
    afsk_freq_detector_t *space_det;
};

afsk_signal_processor_t *afsk_signal_processor_create(size_t sample_rate, size_t mark_freq,
                                                       size_t space_freq, size_t bit_rate,
                                                       size_t window_size) {
    afsk_signal_processor_t *p = (afsk_signal_processor_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;

    if (bit_rate == 0) {
        ESP_LOGE(TAG, "Bit rate cannot be zero");
        free(p);
        return NULL;
    }
    if (sample_rate % bit_rate != 0) {
        ESP_LOGW(TAG, "Sample rate %zu is not divisible by bit rate %zu", sample_rate, bit_rate);
    }

    float nm = (float)mark_freq / (float)sample_rate;
    float ns = (float)space_freq / (float)sample_rate;
    p->mark_det = afsk_freq_detector_create(nm, window_size);
    p->space_det = afsk_freq_detector_create(ns, window_size);
    if (!p->mark_det || !p->space_det) {
        afsk_freq_detector_destroy(p->mark_det);
        afsk_freq_detector_destroy(p->space_det);
        free(p);
        return NULL;
    }
    p->input_buffer_size = window_size;
    rbf_init(&p->input_buffer, window_size);
    p->output_sample_count = 0;
    p->samples_per_bit = sample_rate / bit_rate;
    return p;
}

void afsk_signal_processor_destroy(afsk_signal_processor_t *p) {
    if (!p) return;
    afsk_freq_detector_destroy(p->mark_det);
    afsk_freq_detector_destroy(p->space_det);
    rbf_free(&p->input_buffer);
    free(p);
}

size_t afsk_signal_processor_process(afsk_signal_processor_t *p,
                                      const float *samples, size_t num_samples,
                                      float *out_probs, size_t max_out) {
    if (!p || !samples) return 0;
    size_t n = 0;
    for (size_t i = 0; i < num_samples && n < max_out; i++) {
        if (p->input_buffer.count < p->input_buffer_size) {
            rbf_push(&p->input_buffer, samples[i]);
        } else {
            rbf_pop_front(&p->input_buffer);
            rbf_push(&p->input_buffer, samples[i]);
            p->output_sample_count++;

            if (p->output_sample_count >= p->samples_per_bit) {
                for (size_t j = 0; j < p->input_buffer.count; j++) {
                    float s = rbf_at(&p->input_buffer, j);
                    afsk_freq_detector_process_sample(p->mark_det, s);
                    afsk_freq_detector_process_sample(p->space_det, s);
                }
                float ma = afsk_freq_detector_get_amplitude(p->mark_det);
                float sa = afsk_freq_detector_get_amplitude(p->space_det);
                out_probs[n++] = ma / (sa + ma + FLT_EPSILON);

                afsk_freq_detector_reset(p->mark_det);
                afsk_freq_detector_reset(p->space_det);
                p->output_sample_count = 0;
            }
        }
    }
    return n;
}

/* ============================================================
 * AudioDataBuffer - bit-level state machine for framed data
 * reception with start/end pattern detection and checksum
 * ============================================================ */

/* \x01\x02 = 00000001 00000010 */
static const uint8_t DEFAULT_START_PATTERN[] = {
    0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0
};
/* \x03\x04 = 00000011 00000100 */
static const uint8_t DEFAULT_END_PATTERN[] = {
    0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0
};

#define DEFAULT_START_LEN 16
#define DEFAULT_END_LEN   16

typedef enum {
    STATE_INACTIVE = 0,
    STATE_WAITING,
    STATE_RECEIVING,
} data_state_t;

struct afsk_data_buffer {
    data_state_t state;
    ringbuf_u8_t id_buf;
    size_t id_buf_size;
    uint8_t *bits;
    size_t bit_count;
    size_t max_bits;
    uint8_t *start_pat;
    size_t start_len;
    uint8_t *end_pat;
    size_t end_len;
    bool checksum_on;
    char *decoded_text;
};

static void buf_clear(afsk_data_buffer_t *b) {
    rbu8_clear(&b->id_buf);
    b->bit_count = 0;
}

static uint8_t calc_checksum(const char *s, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += (uint8_t)s[i];
    return sum;
}

static size_t bits_to_bytes(const uint8_t *bits, size_t nbits,
                             uint8_t *out, size_t max_out) {
    size_t n = nbits / 8;
    if (n > max_out) n = max_out;
    for (size_t i = 0; i < n; i++) {
        uint8_t v = 0;
        for (size_t j = 0; j < 8; j++)
            v |= (uint8_t)(bits[i * 8 + j] << (7 - j));
        out[i] = v;
    }
    return n;
}

static afsk_data_buffer_t *buf_alloc(const uint8_t *sp, size_t sl,
                                      const uint8_t *ep, size_t el,
                                      size_t max_bits, bool cksum) {
    afsk_data_buffer_t *b = (afsk_data_buffer_t *)calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->state = STATE_INACTIVE;
    b->checksum_on = cksum;
    b->max_bits = max_bits;

    b->start_len = sl;
    b->start_pat = (uint8_t *)malloc(sl);
    if (b->start_pat) memcpy(b->start_pat, sp, sl);

    b->end_len = el;
    b->end_pat = (uint8_t *)malloc(el);
    if (b->end_pat) memcpy(b->end_pat, ep, el);

    b->id_buf_size = (sl > el) ? sl : el;
    rbu8_init(&b->id_buf, b->id_buf_size);

    /* Extra space for bits pushed before overflow check triggers */
    b->bits = (uint8_t *)malloc(max_bits + b->id_buf_size + 1);
    b->bit_count = 0;

    if (!b->start_pat || !b->end_pat || !b->id_buf.data || !b->bits) {
        afsk_data_buffer_destroy(b);
        return NULL;
    }
    return b;
}

afsk_data_buffer_t *afsk_data_buffer_create(void) {
    /* 776 bits = (32+1+63+1)*8 for SSID(32) + checksum(1) + password(63) + newline(1) */
    return buf_alloc(DEFAULT_START_PATTERN, DEFAULT_START_LEN,
                      DEFAULT_END_PATTERN, DEFAULT_END_LEN, 776, true);
}

afsk_data_buffer_t *afsk_data_buffer_create_ex(size_t max_byte_size,
                                                const uint8_t *start_id, size_t start_id_len,
                                                const uint8_t *end_id, size_t end_id_len,
                                                bool enable_checksum) {
    return buf_alloc(start_id, start_id_len, end_id, end_id_len,
                      max_byte_size * 8, enable_checksum);
}

void afsk_data_buffer_destroy(afsk_data_buffer_t *b) {
    if (!b) return;
    rbu8_free(&b->id_buf);
    free(b->bits);
    free(b->start_pat);
    free(b->end_pat);
    free(b->decoded_text);
    free(b);
}

const char *afsk_data_buffer_get_decoded_text(const afsk_data_buffer_t *b) {
    return b ? b->decoded_text : NULL;
}

void afsk_data_buffer_clear_decoded_text(afsk_data_buffer_t *b) {
    if (!b) return;
    free(b->decoded_text);
    b->decoded_text = NULL;
}

bool afsk_data_buffer_process(afsk_data_buffer_t *b, const float *probs,
                               size_t num_probs, float threshold) {
    if (!b || !probs) return false;

    for (size_t i = 0; i < num_probs; i++) {
        uint8_t bit = (probs[i] > threshold) ? 1 : 0;
        rbu8_push(&b->id_buf, bit);

        switch (b->state) {
        case STATE_INACTIVE:
            if (b->id_buf.count >= b->start_len) {
                b->state = STATE_WAITING;
                ESP_LOGI(TAG, "Entering Waiting state");
            }
            break;

        case STATE_WAITING:
            if (b->id_buf.count >= b->start_len &&
                rbu8_equals(&b->id_buf, b->start_pat, b->start_len)) {
                buf_clear(b);
                b->state = STATE_RECEIVING;
                ESP_LOGI(TAG, "Entering Receiving state");
            }
            break;

        case STATE_RECEIVING:
            b->bits[b->bit_count++] = bit;

            if (b->id_buf.count >= b->end_len) {
                if (rbu8_equals(&b->id_buf, b->end_pat, b->end_len)) {
                    b->state = STATE_INACTIVE;

                    size_t max_bytes = b->bit_count / 8 + 1;
                    uint8_t *bytes = (uint8_t *)malloc(max_bytes);
                    if (!bytes) { buf_clear(b); return false; }

                    size_t nb = bits_to_bytes(b->bits, b->bit_count, bytes, max_bytes);
                    size_t min_len = b->checksum_on
                        ? 1 + b->start_len / 8
                        : b->start_len / 8;

                    uint8_t rx_cksum = 0;
                    if (b->checksum_on && nb >= min_len) {
                        rx_cksum = bytes[nb - b->start_len / 8 - 1];
                    }

                    if (nb < min_len) {
                        free(bytes);
                        buf_clear(b);
                        ESP_LOGW(TAG, "Data too short, clearing buffer");
                        return false;
                    }

                    size_t tlen = nb - min_len;
                    char *txt = (char *)malloc(tlen + 1);
                    if (!txt) { free(bytes); buf_clear(b); return false; }
                    memcpy(txt, bytes, tlen);
                    txt[tlen] = '\0';
                    free(bytes);

                    if (b->checksum_on) {
                        uint8_t calc = calc_checksum(txt, tlen);
                        if (calc != rx_cksum) {
                            ESP_LOGW(TAG, "Checksum mismatch: expected %d, got %d",
                                     rx_cksum, calc);
                            free(txt);
                            buf_clear(b);
                            return false;
                        }
                    }

                    buf_clear(b);
                    free(b->decoded_text);
                    b->decoded_text = txt;
                    return true;

                } else if (b->bit_count >= b->max_bits) {
                    buf_clear(b);
                    ESP_LOGW(TAG, "Buffer overflow, clearing buffer");
                    b->state = STATE_INACTIVE;
                }
            }
            break;
        }
    }
    return false;
}

/* ============================================================
 * Main WiFi credential receive loop via AFSK audio
 * ============================================================ */

#define RX_INPUT_RATE    16000
#define RX_READ_SAMPLES  480
#define RX_MAX_AUDIO     960   /* stereo worst case */
#define RX_MAX_DS        480   /* max downsampled samples */
#define RX_MAX_PROBS     16

void afsk_receive_wifi_credentials(struct app_context *app_ctx,
                                    struct display_t *display,
                                    const afsk_wifi_callbacks_t *cb,
                                    size_t input_channels) {
    const float ds_step = (float)RX_INPUT_RATE / (float)AFSK_AUDIO_SAMPLE_RATE;

    afsk_signal_processor_t *proc = afsk_signal_processor_create(
        AFSK_AUDIO_SAMPLE_RATE, AFSK_MARK_FREQUENCY,
        AFSK_SPACE_FREQUENCY, AFSK_BIT_RATE, AFSK_WINDOW_SIZE);
    afsk_data_buffer_t *dbuf = afsk_data_buffer_create();
    int16_t *abuf = (int16_t *)malloc(RX_MAX_AUDIO * sizeof(int16_t));
    float *dsbuf = (float *)malloc(RX_MAX_DS * sizeof(float));
    float *pbuf = (float *)malloc(RX_MAX_PROBS * sizeof(float));

    if (!proc || !dbuf || !abuf || !dsbuf || !pbuf) {
        ESP_LOGE(TAG, "Failed to allocate AFSK processing structures");
        goto done;
    }

    for (;;) {
        if (app_get_device_state(app_ctx) != kDeviceStateWifiConfiguring) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t got = 0;
        if (!cb->read_audio(cb->ctx, abuf, RX_MAX_AUDIO, &got,
                             RX_INPUT_RATE, RX_READ_SAMPLES)) {
            ESP_LOGI(TAG, "Failed to read audio data, retrying.");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Stereo to mono: keep left channel only */
        size_t mono = got;
        if (input_channels == 2) {
            mono = got / 2;
            for (size_t i = 0, j = 0; i < mono; i++, j += 2)
                abuf[i] = abuf[j];
        }

        /* Downsample from input rate to AFSK processing rate */
        size_t ds_n = 0;
        if (ds_step > 1.0f) {
            size_t last = 0;
            for (size_t i = 0; i < mono && ds_n < RX_MAX_DS; i++) {
                size_t si = (size_t)((float)i / ds_step);
                if (si + 1 > last) {
                    dsbuf[ds_n++] = (float)abuf[i];
                    last = si + 1;
                }
            }
        } else {
            for (size_t i = 0; i < mono && ds_n < RX_MAX_DS; i++)
                dsbuf[ds_n++] = (float)abuf[i];
        }

        size_t np = afsk_signal_processor_process(proc, dsbuf, ds_n, pbuf, RX_MAX_PROBS);

        if (np > 0 && afsk_data_buffer_process(dbuf, pbuf, np, 0.5f)) {
            const char *text = afsk_data_buffer_get_decoded_text(dbuf);
            if (text) {
                ESP_LOGI(TAG, "Received text data: %s", text);
                display_set_chat_message(display, "system", text);

                const char *nl = strchr(text, '\n');
                if (nl) {
                    size_t slen = (size_t)(nl - text);
                    char *ssid = (char *)malloc(slen + 1);
                    if (ssid) {
                        memcpy(ssid, text, slen);
                        ssid[slen] = '\0';
                        const char *pw = nl + 1;
                        ESP_LOGI(TAG, "WiFi SSID: %s, Password: %s", ssid, pw);

                        cb->save_wifi_credentials(cb->ctx, ssid, pw);
                        ESP_LOGI(TAG, "WiFi credentials saved successfully");
                        cb->stop_config_ap(cb->ctx);

                        free(ssid);
                    }
                    afsk_data_buffer_clear_decoded_text(dbuf);
                    goto done;
                } else {
                    ESP_LOGE(TAG, "Invalid data format, no newline character found");
                    afsk_data_buffer_clear_decoded_text(dbuf);
                    continue;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

done:
    free(abuf);
    free(dsbuf);
    free(pbuf);
    afsk_signal_processor_destroy(proc);
    afsk_data_buffer_destroy(dbuf);
}
