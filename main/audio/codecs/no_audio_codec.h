#ifndef _NO_AUDIO_CODEC_H
#define _NO_AUDIO_CODEC_H

#include "audio_codec.h"
#include <driver/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

audio_codec_t *no_audio_codec_duplex_create(int input_sample_rate, int output_sample_rate,
                                             gpio_num_t bclk, gpio_num_t ws,
                                             gpio_num_t dout, gpio_num_t din);

audio_codec_t *no_audio_codec_simplex_create(int input_sample_rate, int output_sample_rate,
                                              gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                              gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din);

audio_codec_t *no_audio_codec_simplex_create_ex(int input_sample_rate, int output_sample_rate,
                                                 gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                                 i2s_std_slot_mask_t spk_slot_mask,
                                                 gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din,
                                                 i2s_std_slot_mask_t mic_slot_mask);

#ifdef __cplusplus
}
#endif

#endif /* _NO_AUDIO_CODEC_H */
