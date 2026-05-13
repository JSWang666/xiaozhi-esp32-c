#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wake_word_detected_cb_t)(const char *wake_word, void *ctx);

typedef struct wake_word_ops {
    bool (*initialize)(void *impl, void *codec, void *models);
    void (*feed)(void *impl, const int16_t *data, size_t samples);
    void (*set_detected_cb)(void *impl, wake_word_detected_cb_t cb, void *ctx);
    void (*start)(void *impl);
    void (*stop)(void *impl);
    size_t (*get_feed_size)(void *impl);
    void (*encode_wake_word_data)(void *impl);
    bool (*get_wake_word_opus)(void *impl, uint8_t *buf, size_t buf_size, size_t *out_len);
    const char *(*get_last_detected)(void *impl);
    void (*destroy)(void *impl);
} wake_word_ops_t;

typedef struct wake_word {
    const wake_word_ops_t *ops;
} wake_word_t;

static inline bool wake_word_initialize(wake_word_t *w, void *codec, void *models) {
    if (w && w->ops && w->ops->initialize) return w->ops->initialize(w, codec, models);
    return false;
}
static inline void wake_word_feed(wake_word_t *w, const int16_t *data, size_t samples) {
    if (w && w->ops && w->ops->feed) w->ops->feed(w, data, samples);
}
static inline void wake_word_set_detected_cb(wake_word_t *w, wake_word_detected_cb_t cb, void *ctx) {
    if (w && w->ops && w->ops->set_detected_cb) w->ops->set_detected_cb(w, cb, ctx);
}
static inline void wake_word_start(wake_word_t *w) {
    if (w && w->ops && w->ops->start) w->ops->start(w);
}
static inline void wake_word_stop(wake_word_t *w) {
    if (w && w->ops && w->ops->stop) w->ops->stop(w);
}
static inline size_t wake_word_get_feed_size(wake_word_t *w) {
    if (w && w->ops && w->ops->get_feed_size) return w->ops->get_feed_size(w);
    return 0;
}
static inline void wake_word_encode_data(wake_word_t *w) {
    if (w && w->ops && w->ops->encode_wake_word_data) w->ops->encode_wake_word_data(w);
}
static inline bool wake_word_get_opus(wake_word_t *w, uint8_t *buf, size_t buf_size, size_t *out_len) {
    if (w && w->ops && w->ops->get_wake_word_opus) return w->ops->get_wake_word_opus(w, buf, buf_size, out_len);
    return false;
}
static inline const char *wake_word_get_last_detected(wake_word_t *w) {
    if (w && w->ops && w->ops->get_last_detected) return w->ops->get_last_detected(w);
    return "";
}
static inline void wake_word_destroy(wake_word_t *w) {
    if (w && w->ops && w->ops->destroy) w->ops->destroy(w);
}

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <string>
#include <vector>
#include <functional>

#include <model_path.h>
#include "audio_codec.h"

class WakeWord {
public:
    virtual ~WakeWord() = default;

    virtual bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) = 0;
    virtual void Feed(const std::vector<int16_t>& data) = 0;
    virtual void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EncodeWakeWordData() = 0;
    virtual bool GetWakeWordOpus(std::vector<uint8_t>& opus) = 0;
    virtual const std::string& GetLastDetectedWakeWord() const = 0;

    wake_word_t *c_wake_word_ = nullptr;
};
#endif

#endif
