#ifndef AUDIO_CODEC_CXX_H
#define AUDIO_CODEC_CXX_H

/** C++ facade over `audio_codec_t`; keep out of pure C translation units. */

#include "audio_codec.h"

#include <functional>
#include <string>
#include <vector>

#include "board.h"

#ifdef __cplusplus

class AudioCodec {
public:
    AudioCodec();
    virtual ~AudioCodec();

    virtual void SetOutputVolume(int volume);
    virtual void SetInputGain(float gain);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);

    virtual void OutputData(std::vector<int16_t> &data);
    virtual bool InputData(std::vector<int16_t> &data);
    virtual void Start();

    inline bool duplex() const { return duplex_; }
    inline bool input_reference() const { return input_reference_; }
    inline int input_sample_rate() const { return input_sample_rate_; }
    inline int output_sample_rate() const { return output_sample_rate_; }
    inline int input_channels() const { return input_channels_; }
    inline int output_channels() const { return output_channels_; }
    inline int output_volume() const { return output_volume_; }
    inline float input_gain() const { return input_gain_; }
    inline bool input_enabled() const { return input_enabled_; }
    inline bool output_enabled() const { return output_enabled_; }

    audio_codec_t *c_codec() { return c_codec_; }

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool duplex_ = false;
    bool input_reference_ = false;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int input_channels_ = 1;
    int output_channels_ = 1;
    int output_volume_ = 70;
    float input_gain_ = 0.0;

    audio_codec_t *c_codec_ = nullptr;

    virtual int Read(int16_t *dest, int samples) = 0;
    virtual int Write(const int16_t *data, int samples) = 0;
};

#endif /* __cplusplus */

#endif /* AUDIO_CODEC_CXX_H */
