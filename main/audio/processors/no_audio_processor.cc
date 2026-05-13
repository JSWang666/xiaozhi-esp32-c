#include "no_audio_processor.h"
#include <esp_log.h>

#define TAG "NoAudioProcessor"

static void output_trampoline(const int16_t *data, size_t samples, void *ctx) {
    auto *p = static_cast<NoAudioProcessor *>(ctx);
    if (p->output_callback_cpp) {
        std::vector<int16_t> vec(data, data + samples);
        p->output_callback_cpp(std::move(vec));
    }
}

static void vad_trampoline(bool speaking, void *ctx) {
    auto *p = static_cast<NoAudioProcessor *>(ctx);
    if (p->vad_callback_cpp) {
        p->vad_callback_cpp(speaking);
    }
}

NoAudioProcessor::NoAudioProcessor() {
    c_processor_ = no_audio_processor_create();
}

NoAudioProcessor::~NoAudioProcessor() {
    if (c_processor_) audio_processor_destroy(c_processor_);
}

void NoAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    audio_processor_initialize(c_processor_, (void *)codec->c_codec_, frame_duration_ms, (void *)models_list);
}

void NoAudioProcessor::Feed(std::vector<int16_t>&& data) {
    audio_processor_feed(c_processor_, data.data(), data.size());
}

void NoAudioProcessor::Start() {
    audio_processor_start(c_processor_);
}

void NoAudioProcessor::Stop() {
    audio_processor_stop(c_processor_);
}

bool NoAudioProcessor::IsRunning() {
    return audio_processor_is_running(c_processor_);
}

void NoAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_cpp = std::move(callback);
    audio_processor_set_output_cb(c_processor_, &output_trampoline, this);
}

void NoAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_callback_cpp = std::move(callback);
    audio_processor_set_vad_cb(c_processor_, &vad_trampoline, this);
}

size_t NoAudioProcessor::GetFeedSize() {
    return audio_processor_get_feed_size(c_processor_);
}

void NoAudioProcessor::EnableDeviceAec(bool enable) {
    audio_processor_enable_device_aec(c_processor_, enable);
}
