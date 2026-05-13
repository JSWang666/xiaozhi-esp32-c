#ifndef AUDIO_DEBUGGER_H
#define AUDIO_DEBUGGER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_debugger audio_debugger_t;

audio_debugger_t *audio_debugger_create(void);
void audio_debugger_destroy(audio_debugger_t *d);
void audio_debugger_feed(audio_debugger_t *d, const int16_t *data, size_t samples);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <vector>

class AudioDebugger {
public:
    AudioDebugger() : impl_(audio_debugger_create()) {}
    ~AudioDebugger() { if (impl_) audio_debugger_destroy(impl_); }

    void Feed(const std::vector<int16_t>& data) {
        audio_debugger_feed(impl_, data.data(), data.size());
    }

private:
    audio_debugger_t *impl_;
};
#endif

#endif
