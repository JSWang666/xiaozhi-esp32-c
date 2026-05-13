#ifndef _DUMMY_AUDIO_CODEC_H
#define _DUMMY_AUDIO_CODEC_H

#include "audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

audio_codec_t *dummy_audio_codec_create(int input_sample_rate, int output_sample_rate);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
class DummyAudioCodec : public AudioCodec {
public:
    DummyAudioCodec(int input_sample_rate, int output_sample_rate) {
        c_codec_ = dummy_audio_codec_create(input_sample_rate, output_sample_rate);
        duplex_ = true;
        input_reference_ = false;
        input_channels_ = 1;
        input_sample_rate_ = input_sample_rate;
        output_sample_rate_ = output_sample_rate;
    }
    ~DummyAudioCodec() override = default;

private:
    int Read(int16_t* dest, int samples) override { return 0; }
    int Write(const int16_t* data, int samples) override { return 0; }
};
#endif

#endif // _DUMMY_AUDIO_CODEC_H
