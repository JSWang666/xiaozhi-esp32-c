#include "audio_codec.h"

#include <stdlib.h>
#include <assert.h>
#include <esp_log.h>
#include <driver/i2c.h>
#include <driver/i2s_tdm.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

#define TAG "Tab5AudioCodec"

typedef struct {
    audio_codec_t base;

    const audio_codec_data_if_t *data_if;
    const audio_codec_ctrl_if_t *out_ctrl_if;
    const audio_codec_if_t *out_codec_if;
    const audio_codec_ctrl_if_t *in_ctrl_if;
    const audio_codec_if_t *in_codec_if;
    const audio_codec_gpio_if_t *gpio_if;

    esp_codec_dev_handle_t output_dev;
    esp_codec_dev_handle_t input_dev;
} tab5_codec_t;

static int tab5_read(audio_codec_t *codec, int16_t *dest, int samples)
{
    tab5_codec_t *c = (tab5_codec_t *)codec;
    if (codec->input_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(c->input_dev, (void *)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

static int tab5_write(audio_codec_t *codec, const int16_t *data, int samples)
{
    tab5_codec_t *c = (tab5_codec_t *)codec;
    if (codec->output_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(c->output_dev, (void *)data, samples * sizeof(int16_t)));
    }
    return samples;
}

static void tab5_set_output_volume(audio_codec_t *codec, int volume)
{
    tab5_codec_t *c = (tab5_codec_t *)codec;
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(c->output_dev, volume));
    audio_codec_base_set_output_volume(codec, volume);
}

static void tab5_enable_input(audio_codec_t *codec, bool enable)
{
    tab5_codec_t *c = (tab5_codec_t *)codec;
    if (enable == codec->input_enabled) return;
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)codec->output_sample_rate,
            .mclk_multiple = 0,
        };
        if (codec->input_reference) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(c->input_dev, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(c->input_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), codec->input_gain));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(c->input_dev));
    }
    audio_codec_base_enable_input(codec, enable);
}

static void tab5_enable_output(audio_codec_t *codec, bool enable)
{
    tab5_codec_t *c = (tab5_codec_t *)codec;
    if (enable == codec->output_enabled) return;
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)codec->output_sample_rate,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(c->output_dev, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(c->output_dev, codec->output_volume));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(c->output_dev));
    }
    audio_codec_base_enable_output(codec, enable);
}

static void tab5_destroy(audio_codec_t *codec)
{
    tab5_codec_t *c = (tab5_codec_t *)codec;
    esp_codec_dev_close(c->output_dev);
    esp_codec_dev_delete(c->output_dev);
    esp_codec_dev_close(c->input_dev);
    esp_codec_dev_delete(c->input_dev);
    audio_codec_delete_codec_if(c->in_codec_if);
    audio_codec_delete_ctrl_if(c->in_ctrl_if);
    audio_codec_delete_codec_if(c->out_codec_if);
    audio_codec_delete_ctrl_if(c->out_ctrl_if);
    audio_codec_delete_gpio_if(c->gpio_if);
    audio_codec_delete_data_if(c->data_if);
    audio_codec_destroy(codec);
    free(c);
}

static const audio_codec_ops_t tab5_ops = {
    .read = tab5_read,
    .write = tab5_write,
    .set_output_volume = tab5_set_output_volume,
    .enable_input = tab5_enable_input,
    .enable_output = tab5_enable_output,
    .destroy = tab5_destroy,
};

static void create_duplex_channels(tab5_codec_t *c,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din)
{
    audio_codec_t *codec = &c->base;
    assert(codec->input_sample_rate == codec->output_sample_rate);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &codec->tx_handle, &codec->rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)codec->output_sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)codec->input_sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(codec->tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(codec->rx_handle, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(codec->tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(codec->rx_handle));
    ESP_LOGI(TAG, "Duplex channels created");
}

audio_codec_t *tab5_audio_codec_create(void *i2c_master_handle,
    int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8388_addr, uint8_t es7210_addr, bool input_reference)
{
    tab5_codec_t *c = calloc(1, sizeof(tab5_codec_t));
    if (!c) return NULL;

    audio_codec_t *codec = &c->base;
    codec->ops = &tab5_ops;
    codec->duplex = true;
    codec->input_reference = input_reference;
    codec->input_channels = input_reference ? 2 : 1;
    codec->input_sample_rate = input_sample_rate;
    codec->output_sample_rate = output_sample_rate;
    codec->output_volume = 70;
    codec->input_gain = 30;

    create_duplex_channels(c, mclk, bclk, ws, dout, din);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = codec->rx_handle,
        .tx_handle = codec->tx_handle,
    };
    c->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(c->data_if);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 1,
        .addr = es8388_addr,
        .bus_handle = i2c_master_handle,
    };
    c->out_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(c->out_ctrl_if);

    c->gpio_if = audio_codec_new_gpio();
    assert(c->gpio_if);

    es8388_codec_cfg_t es8388_cfg = {0};
    es8388_cfg.ctrl_if = c->out_ctrl_if;
    es8388_cfg.gpio_if = c->gpio_if;
    es8388_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8388_cfg.master_mode = true;
    es8388_cfg.pa_pin = -1;
    es8388_cfg.pa_reverted = false;
    es8388_cfg.hw_gain.pa_voltage = 5.0;
    es8388_cfg.hw_gain.codec_dac_voltage = 3.3;
    c->out_codec_if = es8388_codec_new(&es8388_cfg);
    assert(c->out_codec_if);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = c->out_codec_if,
        .data_if = c->data_if,
    };
    c->output_dev = esp_codec_dev_new(&dev_cfg);
    assert(c->output_dev);

    i2c_cfg.addr = es7210_addr;
    c->in_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(c->in_ctrl_if);

    es7210_codec_cfg_t es7210_cfg = {0};
    es7210_cfg.ctrl_if = c->in_ctrl_if;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    c->in_codec_if = es7210_codec_new(&es7210_cfg);
    assert(c->in_codec_if);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = c->in_codec_if;
    c->input_dev = esp_codec_dev_new(&dev_cfg);
    assert(c->input_dev);

    ESP_LOGI(TAG, "Tab5 AudioDevice initialized");
    return codec;
}
