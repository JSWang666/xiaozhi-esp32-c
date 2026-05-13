#ifndef DUMMY_AUDIO_PROCESSOR_H
#define DUMMY_AUDIO_PROCESSOR_H

#include "audio_processor.h"

#ifdef __cplusplus

class NoAudioProcessor : public AudioProcessor {
public:
    NoAudioProcessor();
    ~NoAudioProcessor();

    void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;

    std::function<void(std::vector<int16_t>&& data)> output_callback_cpp;
    std::function<void(bool speaking)> vad_callback_cpp;
};

#endif
#endif
