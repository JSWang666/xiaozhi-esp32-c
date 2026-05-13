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

#ifdef __cplusplus
#include <driver/i2s_pdm.h>
#include <mutex>

class NoAudioCodec : public AudioCodec {
public:
    NoAudioCodec() : AudioCodec() {}
    explicit NoAudioCodec(audio_codec_t *c);
    virtual ~NoAudioCodec();

protected:
    virtual int Write(const int16_t* data, int samples) override;
    virtual int Read(int16_t* dest, int samples) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

private:
    std::mutex data_if_mutex_;
};

class NoAudioCodecDuplex : public NoAudioCodec {
public:
    NoAudioCodecDuplex(int input_sample_rate, int output_sample_rate,
                       gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
};

class NoAudioCodecSimplex : public NoAudioCodec {
public:
    NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate,
                        gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                        gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din);
    NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate,
                        gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                        i2s_std_slot_mask_t spk_slot_mask,
                        gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din,
                        i2s_std_slot_mask_t mic_slot_mask);
};

class NoAudioCodecSimplexPdm : public NoAudioCodec {
public:
    NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate,
                           gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                           gpio_num_t mic_sck, gpio_num_t mic_din);
    NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate,
                           gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                           i2s_std_slot_mask_t spk_slot_mask,
                           gpio_num_t mic_sck, gpio_num_t mic_din);

protected:
    int Read(int16_t* dest, int samples) override;
};

#endif

#endif /* _NO_AUDIO_CODEC_H */
