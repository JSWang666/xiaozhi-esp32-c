#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*audio_processor_output_cb_t)(const int16_t *data, size_t samples, void *ctx);
typedef void (*audio_processor_vad_cb_t)(bool speaking, void *ctx);

typedef struct audio_processor_ops {
    void (*initialize)(void *impl, void *codec, int frame_duration_ms, void *models);
    void (*feed)(void *impl, const int16_t *data, size_t samples);
    void (*start)(void *impl);
    void (*stop)(void *impl);
    bool (*is_running)(void *impl);
    void (*set_output_cb)(void *impl, audio_processor_output_cb_t cb, void *ctx);
    void (*set_vad_cb)(void *impl, audio_processor_vad_cb_t cb, void *ctx);
    size_t (*get_feed_size)(void *impl);
    void (*enable_device_aec)(void *impl, bool enable);
    void (*destroy)(void *impl);
} audio_processor_ops_t;

typedef struct audio_processor {
    const audio_processor_ops_t *ops;
} audio_processor_t;

static inline void audio_processor_initialize(audio_processor_t *p, void *codec, int frame_ms, void *models) {
    if (p && p->ops && p->ops->initialize) p->ops->initialize(p, codec, frame_ms, models);
}
static inline void audio_processor_feed(audio_processor_t *p, const int16_t *data, size_t samples) {
    if (p && p->ops && p->ops->feed) p->ops->feed(p, data, samples);
}
static inline void audio_processor_start(audio_processor_t *p) {
    if (p && p->ops && p->ops->start) p->ops->start(p);
}
static inline void audio_processor_stop(audio_processor_t *p) {
    if (p && p->ops && p->ops->stop) p->ops->stop(p);
}
static inline bool audio_processor_is_running(audio_processor_t *p) {
    if (p && p->ops && p->ops->is_running) return p->ops->is_running(p);
    return false;
}
static inline void audio_processor_set_output_cb(audio_processor_t *p, audio_processor_output_cb_t cb, void *ctx) {
    if (p && p->ops && p->ops->set_output_cb) p->ops->set_output_cb(p, cb, ctx);
}
static inline void audio_processor_set_vad_cb(audio_processor_t *p, audio_processor_vad_cb_t cb, void *ctx) {
    if (p && p->ops && p->ops->set_vad_cb) p->ops->set_vad_cb(p, cb, ctx);
}
static inline size_t audio_processor_get_feed_size(audio_processor_t *p) {
    if (p && p->ops && p->ops->get_feed_size) return p->ops->get_feed_size(p);
    return 0;
}
static inline void audio_processor_enable_device_aec(audio_processor_t *p, bool enable) {
    if (p && p->ops && p->ops->enable_device_aec) p->ops->enable_device_aec(p, enable);
}
static inline void audio_processor_destroy(audio_processor_t *p) {
    if (p && p->ops && p->ops->destroy) p->ops->destroy(p);
}

audio_processor_t *no_audio_processor_create(void);

#if CONFIG_USE_AUDIO_PROCESSOR
audio_processor_t *afe_audio_processor_create(void);
#endif

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <string>
#include <vector>
#include <functional>

#include <model_path.h>
#include "audio_codec_cxx.h"

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    virtual void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) = 0;
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() = 0;
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EnableDeviceAec(bool enable) = 0;

    audio_processor_t *c_processor_ = nullptr;
};
#endif

#endif
