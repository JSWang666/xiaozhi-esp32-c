#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct app_context;
struct display_t;

/* AFSK demodulation constants for acoustic WiFi provisioning */
#define AFSK_AUDIO_SAMPLE_RATE  6400
#define AFSK_MARK_FREQUENCY     1800
#define AFSK_SPACE_FREQUENCY    1500
#define AFSK_BIT_RATE           100
#define AFSK_WINDOW_SIZE        64

/* -------- Opaque handle types -------- */

typedef struct afsk_freq_detector afsk_freq_detector_t;
typedef struct afsk_signal_processor afsk_signal_processor_t;
typedef struct afsk_data_buffer afsk_data_buffer_t;

/* -------- FrequencyDetector (Goertzel algorithm) -------- */

afsk_freq_detector_t *afsk_freq_detector_create(float frequency, size_t window_size);
void afsk_freq_detector_destroy(afsk_freq_detector_t *det);
void afsk_freq_detector_reset(afsk_freq_detector_t *det);
void afsk_freq_detector_process_sample(afsk_freq_detector_t *det, float sample);
float afsk_freq_detector_get_amplitude(const afsk_freq_detector_t *det);

/* -------- AudioSignalProcessor (Mark/Space detection) -------- */

afsk_signal_processor_t *afsk_signal_processor_create(size_t sample_rate, size_t mark_freq,
                                                       size_t space_freq, size_t bit_rate,
                                                       size_t window_size);
void afsk_signal_processor_destroy(afsk_signal_processor_t *proc);

/**
 * Process audio samples and output mark-frequency probabilities.
 * @param out_probs  Caller-allocated output buffer (size >= num_samples / samples_per_bit + 1)
 * @param max_out    Capacity of out_probs
 * @return Number of probability values written to out_probs
 */
size_t afsk_signal_processor_process(afsk_signal_processor_t *proc,
                                      const float *samples, size_t num_samples,
                                      float *out_probs, size_t max_out);

/* -------- AudioDataBuffer (framed data reception) -------- */

afsk_data_buffer_t *afsk_data_buffer_create(void);
afsk_data_buffer_t *afsk_data_buffer_create_ex(size_t max_byte_size,
                                                const uint8_t *start_id, size_t start_id_len,
                                                const uint8_t *end_id, size_t end_id_len,
                                                bool enable_checksum);
void afsk_data_buffer_destroy(afsk_data_buffer_t *buf);

/**
 * Feed probability data into the state machine.
 * @return true when a complete frame has been decoded (retrieve with get_decoded_text)
 */
bool afsk_data_buffer_process(afsk_data_buffer_t *buf,
                               const float *probs, size_t num_probs,
                               float threshold);
const char *afsk_data_buffer_get_decoded_text(const afsk_data_buffer_t *buf);
void afsk_data_buffer_clear_decoded_text(afsk_data_buffer_t *buf);

/* -------- WiFi credential receive via AFSK audio -------- */

/**
 * Callbacks for operations that require C++ singletons.
 * The C implementation calls these via function pointers so it stays pure C.
 */
typedef struct {
    bool (*read_audio)(void *ctx, int16_t *out_data, size_t max_samples,
                       size_t *actual_samples, int sample_rate, int num_samples);
    void (*save_wifi_credentials)(void *ctx, const char *ssid, const char *password);
    void (*stop_config_ap)(void *ctx);
    void *ctx;
} afsk_wifi_callbacks_t;

/**
 * Blocking loop: reads audio, demodulates AFSK, extracts WiFi SSID+password.
 * Returns only after credentials are successfully received and saved.
 */
void afsk_receive_wifi_credentials(struct app_context *app_ctx,
                                    struct display_t *display,
                                    const afsk_wifi_callbacks_t *cb,
                                    size_t input_channels);

#ifdef __cplusplus
}
#endif

/* ================================================================
 * C++ backward-compatibility layer
 *
 * The inline ReceiveWifiCredentialsFromAudio() wraps the pure-C
 * afsk_receive_wifi_credentials() so existing callers in wifi_board.cc
 * continue to compile unchanged.
 * ================================================================ */
#ifdef __cplusplus

#include <vector>
#include <cstring>
#include "wifi_manager.h"
#include "audio_c_api.h"
#include "c_api/app_c_api.h"

static const size_t kAudioSampleRate = AFSK_AUDIO_SAMPLE_RATE;
static const size_t kMarkFrequency   = AFSK_MARK_FREQUENCY;
static const size_t kSpaceFrequency  = AFSK_SPACE_FREQUENCY;
static const size_t kBitRate         = AFSK_BIT_RATE;
static const size_t kWindowSize      = AFSK_WINDOW_SIZE;

namespace audio_wifi_config {

namespace detail {

struct CppContext {
    Application *app;
    WifiManager *wifi_manager;
};

inline bool read_audio_cb(void *ctx, int16_t *out_data, size_t max_samples,
                           size_t *actual_samples, int sample_rate, int num_samples) {
    (void)ctx;
    size_t n = audio_service_read_audio_data(audio_service_get_instance(), out_data, max_samples, sample_rate,
                                             num_samples);
    if (n == 0) {
        return false;
    }
    *actual_samples = n;
    return true;
}

inline void save_wifi_cb(void *ctx, const char *ssid, const char *password) {
    (void)ctx;
    SsidManager::GetInstance().AddSsid(ssid, password);
}

inline void stop_config_ap_cb(void *ctx) {
    auto *c = static_cast<CppContext *>(ctx);
    c->wifi_manager->StopConfigAp();
}

} // namespace detail

inline void ReceiveWifiCredentialsFromAudio(Application *app,
                                             WifiManager *wifi_manager,
                                             Display *display,
                                             size_t input_channels = 1) {
    detail::CppContext cpp_ctx = {app, wifi_manager};
    afsk_wifi_callbacks_t cb = {};
    cb.read_audio           = detail::read_audio_cb;
    cb.save_wifi_credentials = detail::save_wifi_cb;
    cb.stop_config_ap       = detail::stop_config_ap_cb;
    cb.ctx                  = &cpp_ctx;

    afsk_receive_wifi_credentials(app_get_context(), display->c_display(),
                                   &cb, input_channels);
}

} // namespace audio_wifi_config

#endif /* __cplusplus */
