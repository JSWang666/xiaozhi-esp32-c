#include "no_audio_codec.h"

#include <esp_log.h>
#include <cmath>
#include <cstring>

#define TAG "NoAudioCodec"

NoAudioCodec::NoAudioCodec(audio_codec_t *c) : AudioCodec() {
    c_codec_ = c;
    if (c) {
        tx_handle_ = c->tx_handle;
        rx_handle_ = c->rx_handle;
        duplex_ = c->duplex;
        input_reference_ = c->input_reference;
        input_sample_rate_ = c->input_sample_rate;
        output_sample_rate_ = c->output_sample_rate;
        input_channels_ = c->input_channels;
        output_channels_ = c->output_channels;
        output_volume_ = c->output_volume;
        input_gain_ = c->input_gain;
    }
}

NoAudioCodec::~NoAudioCodec() {
    if (c_codec_) {
        audio_codec_destroy(c_codec_);
        c_codec_ = nullptr;
        tx_handle_ = nullptr;
        rx_handle_ = nullptr;
    } else {
        if (rx_handle_ != nullptr) {
            ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
        }
        if (tx_handle_ != nullptr) {
            ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
        }
    }
}

int NoAudioCodec::Write(const int16_t* data, int samples) {
    if (c_codec_ && c_codec_->ops && c_codec_->ops->write) {
        return c_codec_->ops->write(c_codec_, data, samples);
    }
    return 0;
}

int NoAudioCodec::Read(int16_t* dest, int samples) {
    if (c_codec_ && c_codec_->ops && c_codec_->ops->read) {
        return c_codec_->ops->read(c_codec_, dest, samples);
    }
    return 0;
}

void NoAudioCodec::EnableInput(bool enable) {
    if (c_codec_ && c_codec_->ops && c_codec_->ops->enable_input) {
        c_codec_->ops->enable_input(c_codec_, enable);
        input_enabled_ = c_codec_->input_enabled;
        return;
    }
}

void NoAudioCodec::EnableOutput(bool enable) {
    if (c_codec_ && c_codec_->ops && c_codec_->ops->enable_output) {
        c_codec_->ops->enable_output(c_codec_, enable);
        output_enabled_ = c_codec_->output_enabled;
        return;
    }
}

NoAudioCodecDuplex::NoAudioCodecDuplex(int input_sample_rate, int output_sample_rate,
                                        gpio_num_t bclk, gpio_num_t ws,
                                        gpio_num_t dout, gpio_num_t din)
    : NoAudioCodec(no_audio_codec_duplex_create(input_sample_rate, output_sample_rate,
                                                 bclk, ws, dout, din)) {}

NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate,
                                          gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                          gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din)
    : NoAudioCodec(no_audio_codec_simplex_create(input_sample_rate, output_sample_rate,
                                                  spk_bclk, spk_ws, spk_dout,
                                                  mic_sck, mic_ws, mic_din)) {}

NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate,
                                          gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                          i2s_std_slot_mask_t spk_slot_mask,
                                          gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din,
                                          i2s_std_slot_mask_t mic_slot_mask)
    : NoAudioCodec(no_audio_codec_simplex_create_ex(input_sample_rate, output_sample_rate,
                                                     spk_bclk, spk_ws, spk_dout, spk_slot_mask,
                                                     mic_sck, mic_ws, mic_din, mic_slot_mask)) {}

/* --- NoAudioCodecSimplexPdm (still C++ for PDM-specific boards) --- */

NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate,
                                                 gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                                 gpio_num_t mic_sck, gpio_num_t mic_din)
    : NoAudioCodecSimplexPdm(input_sample_rate, output_sample_rate, spk_bclk, spk_ws, spk_dout,
                              I2S_STD_SLOT_LEFT, mic_sck, mic_din) {}

NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate,
                                                 gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                                 i2s_std_slot_mask_t spk_slot_mask,
                                                 gpio_num_t mic_sck, gpio_num_t mic_din)
    : NoAudioCodec()
{
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)1, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
    tx_chan_cfg.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    tx_chan_cfg.auto_clear_after_cb = true;
    tx_chan_cfg.auto_clear_before_cb = false;
    tx_chan_cfg.intr_priority = 0;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle_, NULL));

    i2s_std_config_t tx_std_cfg = {};
    tx_std_cfg.clk_cfg.sample_rate_hz = (uint32_t)output_sample_rate_;
    tx_std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    tx_std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tx_std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    tx_std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    tx_std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    tx_std_cfg.slot_cfg.slot_mask = spk_slot_mask;
    tx_std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    tx_std_cfg.slot_cfg.ws_pol = false;
    tx_std_cfg.slot_cfg.bit_shift = true;
#ifdef I2S_HW_VERSION_2
    tx_std_cfg.slot_cfg.left_align = true;
    tx_std_cfg.slot_cfg.big_endian = false;
    tx_std_cfg.slot_cfg.bit_order_lsb = false;
#endif
    tx_std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    tx_std_cfg.gpio_cfg.bclk = spk_bclk;
    tx_std_cfg.gpio_cfg.ws = spk_ws;
    tx_std_cfg.gpio_cfg.dout = spk_dout;
    tx_std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_std_cfg));

#if SOC_I2S_SUPPORTS_PDM_RX
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle_));
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)input_sample_rate_),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = mic_sck,
            .din = mic_din,
            .invert_flags = { .clk_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle_, &pdm_rx_cfg));
#else
    ESP_LOGE(TAG, "PDM is not supported");
#endif
    ESP_LOGI(TAG, "Simplex PDM channels created");
}

int NoAudioCodecSimplexPdm::Read(int16_t* dest, int samples) {
    size_t bytes_read;
    if (i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Read Failed!");
        return 0;
    }
    samples = bytes_read / sizeof(int16_t);
    if (input_gain_ > 0) {
        int gain_factor = (int)input_gain_;
        for (int i = 0; i < samples; i++) {
            int32_t amplified = dest[i] * gain_factor;
            dest[i] = (amplified > INT16_MAX) ? INT16_MAX : (amplified < -INT16_MAX) ? -INT16_MAX : (int16_t)amplified;
        }
    }
    return samples;
}
