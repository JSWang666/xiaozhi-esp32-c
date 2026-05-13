#ifndef OGG_DEMUXER_H_
#define OGG_DEMUXER_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ogg_demuxer_cb_t)(const uint8_t *data, int sample_rate, size_t len, void *ctx);

typedef struct ogg_demuxer ogg_demuxer_t;

ogg_demuxer_t *ogg_demuxer_create(void);
void ogg_demuxer_destroy(ogg_demuxer_t *d);
void ogg_demuxer_reset(ogg_demuxer_t *d);
size_t ogg_demuxer_process(ogg_demuxer_t *d, const uint8_t *data, size_t size);
void ogg_demuxer_set_callback(ogg_demuxer_t *d, ogg_demuxer_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <functional>

class OggDemuxer {
public:
    OggDemuxer() : impl_(ogg_demuxer_create()) {}
    ~OggDemuxer() { if (impl_) ogg_demuxer_destroy(impl_); }

    void Reset() { ogg_demuxer_reset(impl_); }
    size_t Process(const uint8_t *data, size_t size) {
        return ogg_demuxer_process(impl_, data, size);
    }

    void OnDemuxerFinished(std::function<void(const uint8_t *, int, size_t)> cb) {
        cpp_callback_ = std::move(cb);
        ogg_demuxer_set_callback(impl_, &trampoline, this);
    }

    ogg_demuxer_t *c_impl() { return impl_; }

private:
    static void trampoline(const uint8_t *data, int sr, size_t len, void *ctx) {
        auto *self = static_cast<OggDemuxer *>(ctx);
        if (self->cpp_callback_) self->cpp_callback_(data, sr, len);
    }
    ogg_demuxer_t *impl_;
    std::function<void(const uint8_t *, int, size_t)> cpp_callback_;
};
#endif

#endif /* OGG_DEMUXER_H_ */
