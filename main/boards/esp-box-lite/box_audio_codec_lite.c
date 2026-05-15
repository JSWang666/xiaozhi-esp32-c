#include "audio_codec.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

static const char TAG[] = "BoxAudioCodecLite";

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

    int16_t *ref_buffer;
    int ref_buffer_size;
    int read_pos;
    int write_pos;
} box_lite_codec_t;

static void create_duplex_channels(box_lite_codec_t *c,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
    gpio_num_t dout, gpio_num_t din)
{
    assert(c->base.input_sample_rate == c->base.output_sample_rate);

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
            .sample_rate_hz = (uint32_t)c->base.output_sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)c->base.input_sample_rate,
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
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(c->base.tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(c->base.rx_handle, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(c->base.tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(c->base.rx_handle));
    ESP_LOGI(TAG, "Duplex channels created");
}

static int lite_read(audio_codec_t *codec, int16_t *dest, int samples)
{
    box_lite_codec_t *c = (box_lite_codec_t *)codec;
    if (codec->input_enabled) {
        if (!codec->input_reference) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(c->input_dev, (void *)dest, samples * sizeof(int16_t)));
        } else {
            int channels = codec->input_channels - codec->input_reference;
            int size = samples / codec->input_channels;
            int16_t *data = malloc(size * channels * sizeof(int16_t));
            if (!data) return samples;
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(c->input_dev, (void *)data, size * channels * sizeof(int16_t)));
            int j = 0, i = 0;
            while (i < samples) {
                for (int p = 0; p < channels; p++) {
                    dest[i++] = data[j++];
                }
                dest[i++] = c->read_pos < c->write_pos ? c->ref_buffer[c->read_pos++] : 0;
            }
            if (c->read_pos == c->write_pos) {
                c->read_pos = c->write_pos = 0;
            }
            free(data);
        }
    }
    return samples;
}

static int lite_write(audio_codec_t *codec, const int16_t *data, int samples)
{
    box_lite_codec_t *c = (box_lite_codec_t *)codec;
    if (codec->output_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(c->output_dev, (void *)data, samples * sizeof(int16_t)));
        if (codec->input_reference) {
            if (c->write_pos - c->read_pos + samples > c->ref_buffer_size) {
                assert(c->ref_buffer_size >= samples);
                c->read_pos = c->write_pos + samples - c->ref_buffer_size;
            }
            if (c->read_pos) {
                if (c->write_pos != c->read_pos) {
                    memmove(c->ref_buffer, c->ref_buffer + c->read_pos,
                            (c->write_pos - c->read_pos) * sizeof(int16_t));
                }
                c->write_pos -= c->read_pos;
                c->read_pos = 0;
            }
            memcpy(&c->ref_buffer[c->write_pos], data, samples * sizeof(int16_t));
            c->write_pos += samples;
        }
    }
    return samples;
}

static void lite_set_output_volume(audio_codec_t *codec, int volume)
{
    box_lite_codec_t *c = (box_lite_codec_t *)codec;
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(c->output_dev, volume));
    audio_codec_base_set_output_volume(codec, volume);
}

static void lite_enable_input(audio_codec_t *codec, bool enable)
{
    box_lite_codec_t *c = (box_lite_codec_t *)codec;
    if (enable == codec->input_enabled) return;
    if (enable) {
        int channels = codec->input_channels - codec->input_reference;
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = (uint8_t)channels,
            .channel_mask = 0,
            .sample_rate = (uint32_t)codec->input_sample_rate,
            .mclk_multiple = 0,
        };
        for (int i = 0; i < fs.channel; i++) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(i);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(c->input_dev, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(c->input_dev, 37.5));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(c->input_dev));
    }
    audio_codec_base_enable_input(codec, enable);
}

static void lite_enable_output(audio_codec_t *codec, bool enable)
{
    box_lite_codec_t *c = (box_lite_codec_t *)codec;
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

static void lite_destroy(audio_codec_t *codec)
{
    box_lite_codec_t *c = (box_lite_codec_t *)codec;
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

    free(c->ref_buffer);
    free(c);
}

static const audio_codec_ops_t lite_ops = {
    .read = lite_read,
    .write = lite_write,
    .set_output_volume = lite_set_output_volume,
    .enable_input = lite_enable_input,
    .enable_output = lite_enable_output,
    .destroy = lite_destroy,
};

audio_codec_t *box_codec_lite_create(void *i2c_master_handle,
    int input_sample_rate, int output_sample_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, bool input_reference)
{
    box_lite_codec_t *c = calloc(1, sizeof(box_lite_codec_t));
    if (!c) return NULL;

    c->base.ops = &lite_ops;
    c->base.duplex = true;
    c->base.input_reference = input_reference;
    c->base.input_channels = 2 + input_reference;
    c->base.input_sample_rate = input_sample_rate;
    c->base.output_sample_rate = output_sample_rate;
    c->base.output_volume = 70;

    if (input_reference) {
        c->ref_buffer_size = 960 * 2;
        c->ref_buffer = calloc(c->ref_buffer_size, sizeof(int16_t));
    }

    create_duplex_channels(c, (gpio_num_t)mclk, (gpio_num_t)bclk,
        (gpio_num_t)ws, (gpio_num_t)dout, (gpio_num_t)din);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = c->base.rx_handle,
        .tx_handle = c->base.tx_handle,
    };
    c->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(c->data_if != NULL);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 1,
        .addr = ES8156_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_master_handle,
    };
    c->out_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(c->out_ctrl_if != NULL);

    c->gpio_if = audio_codec_new_gpio();
    assert(c->gpio_if != NULL);

    es8156_codec_cfg_t cfg = {
        .ctrl_if = c->out_ctrl_if,
        .gpio_if = c->gpio_if,
        .pa_pin = (gpio_num_t)pa_pin,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
    };
    c->out_codec_if = es8156_codec_new(&cfg);
    assert(c->out_codec_if != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = c->out_codec_if,
        .data_if = c->data_if,
    };
    c->output_dev = esp_codec_dev_new(&dev_cfg);
    assert(c->output_dev != NULL);

    i2c_cfg.addr = ES7243E_CODEC_DEFAULT_ADDR;
    c->in_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(c->in_ctrl_if != NULL);

    es7243e_codec_cfg_t es7243_cfg = {
        .ctrl_if = c->in_ctrl_if,
    };
    c->in_codec_if = es7243e_codec_new(&es7243_cfg);
    assert(c->in_codec_if != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = c->in_codec_if;
    c->input_dev = esp_codec_dev_new(&dev_cfg);
    assert(c->input_dev != NULL);

    ESP_LOGI(TAG, "BoxAudioCodecLite initialized");
    return &c->base;
}
