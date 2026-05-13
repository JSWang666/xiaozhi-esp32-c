#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include <stdbool.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <driver/i2s_std.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

typedef struct audio_codec_t audio_codec_t;

typedef struct audio_codec_ops {
    int  (*read)(audio_codec_t *codec, int16_t *dest, int samples);
    int  (*write)(audio_codec_t *codec, const int16_t *data, int samples);
    void (*set_output_volume)(audio_codec_t *codec, int volume);
    void (*set_input_gain)(audio_codec_t *codec, float gain);
    void (*enable_input)(audio_codec_t *codec, bool enable);
    void (*enable_output)(audio_codec_t *codec, bool enable);
    void (*start)(audio_codec_t *codec);
    void (*destroy)(audio_codec_t *codec);
} audio_codec_ops_t;

struct audio_codec_t {
    const audio_codec_ops_t *ops;

    i2s_chan_handle_t tx_handle;
    i2s_chan_handle_t rx_handle;

    bool duplex;
    bool input_reference;
    bool input_enabled;
    bool output_enabled;
    int input_sample_rate;
    int output_sample_rate;
    int input_channels;
    int output_channels;
    int output_volume;
    float input_gain;
};

void audio_codec_base_init(audio_codec_t *codec);
void audio_codec_base_start(audio_codec_t *codec);
void audio_codec_base_set_output_volume(audio_codec_t *codec, int volume);
void audio_codec_base_set_input_gain(audio_codec_t *codec, float gain);
void audio_codec_base_enable_input(audio_codec_t *codec, bool enable);
void audio_codec_base_enable_output(audio_codec_t *codec, bool enable);
void audio_codec_output_data(audio_codec_t *codec, int16_t *data, int samples);
bool audio_codec_input_data(audio_codec_t *codec, int16_t *data, int samples);
void audio_codec_destroy(audio_codec_t *codec);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <vector>
#include <string>
#include <functional>

#include "board.h"

class AudioCodec {
public:
    AudioCodec();
    virtual ~AudioCodec();

    virtual void SetOutputVolume(int volume);
    virtual void SetInputGain(float gain);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);

    virtual void OutputData(std::vector<int16_t>& data);
    virtual bool InputData(std::vector<int16_t>& data);
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

    virtual int Read(int16_t* dest, int samples) = 0;
    virtual int Write(const int16_t* data, int samples) = 0;
};
#endif

#endif /* _AUDIO_CODEC_H */
