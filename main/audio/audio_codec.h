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

#endif /* _AUDIO_CODEC_H */
