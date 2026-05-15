#include "audio_codec.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "config.h"

static const char TAG[] = "K10AudioCodec";

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

typedef struct {
    audio_codec_t base;
    const audio_codec_data_if_t *data_if;
    esp_codec_dev_handle_t input_dev;
    bool input_reference;
} k10_codec_t;

static int k10_read(audio_codec_t *codec, int16_t *dest, int samples)
{
    k10_codec_t *c = (k10_codec_t *)codec;
    if (codec->input_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(c->input_dev, (void *)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

static int k10_write(audio_codec_t *codec, const int16_t *data, int samples)
{
    if (!codec->output_enabled)
        return samples;

    int32_t *buffer = (int32_t *)malloc(samples * 2 * sizeof(int32_t));
    if (!buffer) return samples;

    int32_t volume_factor = (int32_t)(pow((double)codec->output_volume / 100.0, 2) * 65536);
    for (int i = 0; i < samples; i++) {
        int64_t temp = (int64_t)data[i] * volume_factor;
        int32_t val;
        if (temp > INT32_MAX)
            val = INT32_MAX;
        else if (temp < INT32_MIN)
            val = INT32_MIN;
        else
            val = (int32_t)temp;
        buffer[i * 2] = val;
        buffer[i * 2 + 1] = val;
    }

    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(codec->tx_handle, buffer, samples * 2 * sizeof(int32_t), &bytes_written, portMAX_DELAY));
    free(buffer);
    return (int)(bytes_written / sizeof(int32_t));
}

static void k10_set_output_volume(audio_codec_t *codec, int volume)
{
    audio_codec_base_set_output_volume(codec, volume);
}

static void k10_enable_input(audio_codec_t *codec, bool enable)
{
    k10_codec_t *c = (k10_codec_t *)codec;
    if (enable == codec->input_enabled) return;

    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)codec->output_sample_rate,
            .mclk_multiple = 0,
        };
        if (c->input_reference) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(c->input_dev, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(c->input_dev, 37.5));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(c->input_dev));
    }
    audio_codec_base_enable_input(codec, enable);
}

static void k10_enable_output(audio_codec_t *codec, bool enable)
{
    if (enable == codec->output_enabled) return;
    audio_codec_base_set_output_volume(codec, codec->output_volume);
    audio_codec_base_enable_output(codec, enable);
}

static void k10_start(audio_codec_t *codec)
{
    k10_enable_input(codec, true);
    k10_enable_output(codec, true);
    ESP_LOGI(TAG, "K10 Audio codec started");
}

static void k10_destroy(audio_codec_t *codec)
{
    k10_codec_t *c = (k10_codec_t *)codec;
    if (c->input_dev) {
        esp_codec_dev_close(c->input_dev);
        esp_codec_dev_delete(c->input_dev);
    }
    audio_codec_destroy(codec);
    free(c);
}

static const audio_codec_ops_t k10_ops = {
    .read = k10_read,
    .write = k10_write,
    .set_output_volume = k10_set_output_volume,
    .enable_input = k10_enable_input,
    .enable_output = k10_enable_output,
    .start = k10_start,
    .destroy = k10_destroy,
};

audio_codec_t *k10_audio_codec_create(i2c_master_bus_handle_t i2c_bus)
{
    k10_codec_t *c = calloc(1, sizeof(k10_codec_t));
    if (!c) return NULL;

    c->base.ops = &k10_ops;
    c->base.input_sample_rate = AUDIO_INPUT_SAMPLE_RATE;
    c->base.output_sample_rate = AUDIO_OUTPUT_SAMPLE_RATE;
    c->base.output_volume = 100;
    c->base.duplex = true;
    c->base.input_reference = AUDIO_INPUT_REFERENCE;
    c->base.input_channels = AUDIO_INPUT_REFERENCE ? 2 : 1;
    c->input_reference = AUDIO_INPUT_REFERENCE;

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &c->base.tx_handle, &c->base.rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)AUDIO_OUTPUT_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .bclk = AUDIO_I2S_GPIO_BCLK,
            .ws = AUDIO_I2S_GPIO_WS,
            .dout = AUDIO_I2S_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)AUDIO_INPUT_SAMPLE_RATE,
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
            .mclk = AUDIO_I2S_GPIO_MCLK,
            .bclk = AUDIO_I2S_GPIO_BCLK,
            .ws = AUDIO_I2S_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_I2S_GPIO_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(c->base.tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(c->base.rx_handle, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(c->base.tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(c->base.rx_handle));

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = c->base.rx_handle,
        .tx_handle = c->base.tx_handle,
    };
    c->data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = AUDIO_CODEC_ES7210_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *in_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    es7243e_codec_cfg_t es7243e_cfg = {
        .ctrl_if = in_ctrl_if,
    };
    const audio_codec_if_t *in_codec_if = es7243e_codec_new(&es7243e_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = in_codec_if,
        .data_if = c->data_if,
    };
    c->input_dev = esp_codec_dev_new(&dev_cfg);
    if (!c->input_dev) {
        ESP_LOGE(TAG, "Failed to create input codec device");
        free(c);
        return NULL;
    }

    ESP_LOGI(TAG, "K10 AudioCodec initialized");
    return &c->base;
}
