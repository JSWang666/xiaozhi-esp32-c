#ifndef CODEC_C_API_H
#define CODEC_C_API_H

#include "audio_codec.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

audio_codec_t *es8311_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool use_mclk, bool pa_inverted);

audio_codec_t *es8374_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool use_mclk);

audio_codec_t *es8388_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool input_reference);

audio_codec_t *es8389_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool use_mclk);

audio_codec_t *box_codec_create(void *i2c_handle,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t es8311_addr, uint8_t es7210_addr,
    bool input_reference);

#ifdef __cplusplus
}
#endif

#endif
