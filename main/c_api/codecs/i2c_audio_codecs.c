#include "codec_c_api.h"
#include "settings.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define TAG "I2cAudioCodec"

typedef enum {
    CODEC_ES8311,
    CODEC_ES8374,
    CODEC_ES8388,
    CODEC_ES8389,
    CODEC_BOX,
} i2c_codec_kind_t;

typedef struct {
    audio_codec_t base;
    i2c_codec_kind_t kind;

    const audio_codec_data_if_t *data_if;
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_ctrl_if_t *out_ctrl_if;
    const audio_codec_ctrl_if_t *in_ctrl_if;
    const audio_codec_if_t *codec_if;
    const audio_codec_if_t *out_codec_if;
    const audio_codec_if_t *in_codec_if;
    const audio_codec_gpio_if_t *gpio_if;

    esp_codec_dev_handle_t dev;
    esp_codec_dev_handle_t output_dev;
    esp_codec_dev_handle_t input_dev;
    SemaphoreHandle_t mutex;

    gpio_num_t pa_pin;
    bool pa_inverted;
} i2c_audio_codec_t;

static void codec_lock(i2c_audio_codec_t *c)
{
    if (c->mutex) xSemaphoreTake(c->mutex, portMAX_DELAY);
}

static void codec_unlock(i2c_audio_codec_t *c)
{
    if (c->mutex) xSemaphoreGive(c->mutex);
}

static bool create_std_duplex_channels(i2c_audio_codec_t *c, gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
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
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
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
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(c->base.tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(c->base.rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(c->base.tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(c->base.rx_handle));
    return true;
}

static bool create_box_channels(i2c_audio_codec_t *c, gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
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
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
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
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
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
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
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
    return true;
}

static void i2c_codec_start(audio_codec_t *codec)
{
    settings_t *s = settings_open("audio", false);
    if (s) {
        codec->output_volume = settings_get_int(s, "output_volume", codec->output_volume);
        if (codec->output_volume <= 0) {
            ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", codec->output_volume);
            codec->output_volume = 10;
        }
        settings_close(s);
    }
    ESP_LOGI(TAG, "Audio codec started");
}

static void es8311_update_state(i2c_audio_codec_t *c)
{
    if ((c->base.input_enabled || c->base.output_enabled) && c->dev == NULL) {
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = c->codec_if,
            .data_if = c->data_if,
        };
        c->dev = esp_codec_dev_new(&dev_cfg);
        assert(c->dev != NULL);

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)c->base.input_sample_rate,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(c->dev, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(c->dev, c->base.input_gain));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(c->dev, c->base.output_volume));
    } else if (!c->base.input_enabled && !c->base.output_enabled && c->dev != NULL) {
        (void)esp_codec_dev_close(c->dev);
        esp_codec_dev_delete(c->dev);
        c->dev = NULL;
    }
    if (c->pa_pin != GPIO_NUM_NC) {
        int level = c->base.output_enabled ? 1 : 0;
        gpio_set_level(c->pa_pin, c->pa_inverted ? !level : level);
    }
}

static void i2c_set_output_volume(audio_codec_t *codec, int volume)
{
    i2c_audio_codec_t *c = (i2c_audio_codec_t *)codec;
    codec_lock(c);
    if (c->dev) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(c->dev, volume));
    }
    if (c->output_dev) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(c->output_dev, volume));
    }
    audio_codec_base_set_output_volume(codec, volume);
    codec_unlock(c);
}

static void i2c_set_input_gain(audio_codec_t *codec, float gain)
{
    i2c_audio_codec_t *c = (i2c_audio_codec_t *)codec;
    codec_lock(c);
    if (c->dev) {
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(c->dev, gain));
    }
    if (c->input_dev) {
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(c->input_dev, gain));
    }
    audio_codec_base_set_input_gain(codec, gain);
    codec_unlock(c);
}

static void i2c_enable_input(audio_codec_t *codec, bool enable)
{
    i2c_audio_codec_t *c = (i2c_audio_codec_t *)codec;
    codec_lock(c);
    if (enable == codec->input_enabled) {
        codec_unlock(c);
        return;
    }

    if (c->kind == CODEC_ES8311) {
        audio_codec_base_enable_input(codec, enable);
        es8311_update_state(c);
        codec_unlock(c);
        return;
    }

    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = (c->kind == CODEC_BOX) ? 4 : (uint8_t)codec->input_channels,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)((c->kind == CODEC_BOX) ? codec->output_sample_rate : codec->input_sample_rate),
            .mclk_multiple = 0,
        };
        if (codec->input_reference) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(c->input_dev, &fs));
        if (c->kind == CODEC_ES8388 && codec->input_reference) {
            uint8_t gain = (11 << 4) + 0;
            c->ctrl_if->write_reg(c->ctrl_if, 0x09, 1, &gain, 1);
        } else if (c->kind == CODEC_BOX) {
            ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(c->input_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), codec->input_gain));
        } else {
            ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(c->input_dev, codec->input_gain));
        }
    } else {
        (void)esp_codec_dev_close(c->input_dev);
    }
    audio_codec_base_enable_input(codec, enable);
    codec_unlock(c);
}

static void i2c_enable_output(audio_codec_t *codec, bool enable)
{
    i2c_audio_codec_t *c = (i2c_audio_codec_t *)codec;
    codec_lock(c);
    if (enable == codec->output_enabled) {
        codec_unlock(c);
        return;
    }

    if (c->kind == CODEC_ES8311) {
        audio_codec_base_enable_output(codec, enable);
        es8311_update_state(c);
        codec_unlock(c);
        return;
    }

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
        if (c->kind == CODEC_ES8388) {
            uint8_t reg_val = 30;
            uint8_t regs[] = { 46, 47, 48, 49 };
            for (size_t i = 0; i < sizeof(regs); ++i) {
                c->ctrl_if->write_reg(c->ctrl_if, regs[i], 1, &reg_val, 1);
            }
        }
        if (c->pa_pin != GPIO_NUM_NC && c->kind != CODEC_BOX) {
            gpio_set_level(c->pa_pin, 1);
        }
    } else {
        (void)esp_codec_dev_close(c->output_dev);
        if (c->pa_pin != GPIO_NUM_NC && c->kind != CODEC_BOX) {
            gpio_set_level(c->pa_pin, 0);
        }
    }
    audio_codec_base_enable_output(codec, enable);
    codec_unlock(c);
}

static int i2c_read(audio_codec_t *codec, int16_t *dest, int samples)
{
    i2c_audio_codec_t *c = (i2c_audio_codec_t *)codec;
    if (!codec->input_enabled) return samples;
    esp_codec_dev_handle_t dev = c->dev ? c->dev : c->input_dev;
    if (dev) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(dev, (void *)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

static int i2c_write(audio_codec_t *codec, const int16_t *data, int samples)
{
    i2c_audio_codec_t *c = (i2c_audio_codec_t *)codec;
    if (!codec->output_enabled) return samples;
    esp_codec_dev_handle_t dev = c->dev ? c->dev : c->output_dev;
    if (dev && data) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(dev, (void *)data, samples * sizeof(int16_t)));
    }
    return samples;
}

static void i2c_destroy(audio_codec_t *codec)
{
    i2c_audio_codec_t *c = (i2c_audio_codec_t *)codec;
    if (c->dev) {
        (void)esp_codec_dev_close(c->dev);
        esp_codec_dev_delete(c->dev);
    }
    if (c->output_dev) {
        (void)esp_codec_dev_close(c->output_dev);
        esp_codec_dev_delete(c->output_dev);
    }
    if (c->input_dev) {
        (void)esp_codec_dev_close(c->input_dev);
        esp_codec_dev_delete(c->input_dev);
    }

    if (c->in_codec_if) audio_codec_delete_codec_if(c->in_codec_if);
    if (c->in_ctrl_if) audio_codec_delete_ctrl_if(c->in_ctrl_if);
    if (c->out_codec_if) audio_codec_delete_codec_if(c->out_codec_if);
    if (c->out_ctrl_if) audio_codec_delete_ctrl_if(c->out_ctrl_if);
    if (c->codec_if) audio_codec_delete_codec_if(c->codec_if);
    if (c->ctrl_if) audio_codec_delete_ctrl_if(c->ctrl_if);
    if (c->gpio_if) audio_codec_delete_gpio_if(c->gpio_if);
    if (c->data_if) audio_codec_delete_data_if(c->data_if);
    if (c->mutex) vSemaphoreDelete(c->mutex);
    free(c);
}

static const audio_codec_ops_t i2c_codec_ops = {
    .read = i2c_read,
    .write = i2c_write,
    .set_output_volume = i2c_set_output_volume,
    .set_input_gain = i2c_set_input_gain,
    .enable_input = i2c_enable_input,
    .enable_output = i2c_enable_output,
    .start = i2c_codec_start,
    .destroy = i2c_destroy,
};

static i2c_audio_codec_t *codec_alloc(i2c_codec_kind_t kind, int in_rate, int out_rate, bool input_reference,
                                      int input_channels, float input_gain, gpio_num_t pa_pin)
{
    i2c_audio_codec_t *c = (i2c_audio_codec_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    audio_codec_base_init(&c->base);
    c->base.ops = &i2c_codec_ops;
    c->base.duplex = true;
    c->base.input_reference = input_reference;
    c->base.input_channels = input_channels;
    c->base.output_channels = 1;
    c->base.input_sample_rate = in_rate;
    c->base.output_sample_rate = out_rate;
    c->base.input_gain = input_gain;
    c->kind = kind;
    c->pa_pin = pa_pin;
    c->mutex = xSemaphoreCreateMutex();
    if (!c->mutex) {
        free(c);
        return NULL;
    }
    return c;
}

audio_codec_t *es8311_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool use_mclk, bool pa_inverted)
{
    i2c_audio_codec_t *c = codec_alloc(CODEC_ES8311, in_rate, out_rate, false, 1, 30.0f, (gpio_num_t)pa_pin);
    if (!c) return NULL;
    c->pa_inverted = pa_inverted;
    assert(c->base.input_sample_rate == c->base.output_sample_rate);
    create_std_duplex_channels(c, (gpio_num_t)mclk, (gpio_num_t)bclk, (gpio_num_t)ws, (gpio_num_t)dout, (gpio_num_t)din);

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = c->base.rx_handle, .tx_handle = c->base.tx_handle };
    c->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(c->data_if != NULL);

    audio_codec_i2c_cfg_t i2c_cfg = { .port = (i2c_port_t)i2c_port, .addr = addr, .bus_handle = i2c_handle };
    c->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(c->ctrl_if != NULL);
    c->gpio_if = audio_codec_new_gpio();
    assert(c->gpio_if != NULL);

    es8311_codec_cfg_t cfg = {0};
    cfg.ctrl_if = c->ctrl_if;
    cfg.gpio_if = c->gpio_if;
    cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    cfg.pa_pin = (gpio_num_t)pa_pin;
    cfg.use_mclk = use_mclk;
    cfg.hw_gain.pa_voltage = 5.0;
    cfg.hw_gain.codec_dac_voltage = 3.3;
    cfg.pa_reverted = pa_inverted;
    c->codec_if = es8311_codec_new(&cfg);
    assert(c->codec_if != NULL);
    ESP_LOGI(TAG, "ES8311 codec initialized");
    return &c->base;
}

audio_codec_t *es8374_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool use_mclk)
{
    (void)use_mclk;
    i2c_audio_codec_t *c = codec_alloc(CODEC_ES8374, in_rate, out_rate, false, 1, 30.0f, (gpio_num_t)pa_pin);
    if (!c) return NULL;
    create_std_duplex_channels(c, (gpio_num_t)mclk, (gpio_num_t)bclk, (gpio_num_t)ws, (gpio_num_t)dout, (gpio_num_t)din);

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = c->base.rx_handle, .tx_handle = c->base.tx_handle };
    c->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(c->data_if != NULL);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = (i2c_port_t)i2c_port, .addr = addr, .bus_handle = i2c_handle };
    c->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(c->ctrl_if != NULL);
    c->gpio_if = audio_codec_new_gpio();
    assert(c->gpio_if != NULL);

    es8374_codec_cfg_t cfg = {0};
    cfg.ctrl_if = c->ctrl_if;
    cfg.gpio_if = c->gpio_if;
    cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    cfg.pa_pin = (gpio_num_t)pa_pin;
    c->codec_if = es8374_codec_new(&cfg);
    assert(c->codec_if != NULL);

    esp_codec_dev_cfg_t dev_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = c->codec_if, .data_if = c->data_if };
    c->output_dev = esp_codec_dev_new(&dev_cfg);
    assert(c->output_dev != NULL);
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    c->input_dev = esp_codec_dev_new(&dev_cfg);
    assert(c->input_dev != NULL);
    esp_codec_set_disable_when_closed(c->output_dev, false);
    esp_codec_set_disable_when_closed(c->input_dev, false);
    ESP_LOGI(TAG, "ES8374 codec initialized");
    return &c->base;
}

audio_codec_t *es8388_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool input_reference)
{
    i2c_audio_codec_t *c = codec_alloc(CODEC_ES8388, in_rate, out_rate, input_reference,
                                      input_reference ? 2 : 1, 24.0f, (gpio_num_t)pa_pin);
    if (!c) return NULL;
    create_std_duplex_channels(c, (gpio_num_t)mclk, (gpio_num_t)bclk, (gpio_num_t)ws, (gpio_num_t)dout, (gpio_num_t)din);

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = c->base.rx_handle, .tx_handle = c->base.tx_handle };
    c->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(c->data_if != NULL);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = (i2c_port_t)i2c_port, .addr = addr, .bus_handle = i2c_handle };
    c->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(c->ctrl_if != NULL);
    c->gpio_if = audio_codec_new_gpio();
    assert(c->gpio_if != NULL);

    es8388_codec_cfg_t cfg = {0};
    cfg.ctrl_if = c->ctrl_if;
    cfg.gpio_if = c->gpio_if;
    cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    cfg.master_mode = true;
    cfg.pa_pin = (gpio_num_t)pa_pin;
    cfg.pa_reverted = false;
    cfg.hw_gain.pa_voltage = 5.0;
    cfg.hw_gain.codec_dac_voltage = 3.3;
    c->codec_if = es8388_codec_new(&cfg);
    assert(c->codec_if != NULL);

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = c->codec_if, .data_if = c->data_if };
    c->output_dev = esp_codec_dev_new(&out_cfg);
    assert(c->output_dev != NULL);
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = c->codec_if, .data_if = c->data_if };
    c->input_dev = esp_codec_dev_new(&in_cfg);
    assert(c->input_dev != NULL);
    esp_codec_set_disable_when_closed(c->output_dev, false);
    esp_codec_set_disable_when_closed(c->input_dev, false);
    ESP_LOGI(TAG, "ES8388 codec initialized");
    return &c->base;
}

audio_codec_t *es8389_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool use_mclk)
{
    i2c_audio_codec_t *c = codec_alloc(CODEC_ES8389, in_rate, out_rate, false, 1, 40.0f, (gpio_num_t)pa_pin);
    if (!c) return NULL;
    create_std_duplex_channels(c, (gpio_num_t)mclk, (gpio_num_t)bclk, (gpio_num_t)ws, (gpio_num_t)dout, (gpio_num_t)din);

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = c->base.rx_handle, .tx_handle = c->base.tx_handle };
    c->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(c->data_if != NULL);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = (i2c_port_t)i2c_port, .addr = addr, .bus_handle = i2c_handle };
    c->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(c->ctrl_if != NULL);
    c->gpio_if = audio_codec_new_gpio();
    assert(c->gpio_if != NULL);

    es8389_codec_cfg_t cfg = {0};
    cfg.ctrl_if = c->ctrl_if;
    cfg.gpio_if = c->gpio_if;
    cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    cfg.pa_pin = (gpio_num_t)pa_pin;
    cfg.use_mclk = use_mclk;
    cfg.hw_gain.pa_voltage = 5.0;
    cfg.hw_gain.codec_dac_voltage = 3.3;
    c->codec_if = es8389_codec_new(&cfg);
    assert(c->codec_if != NULL);

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = c->codec_if, .data_if = c->data_if };
    c->output_dev = esp_codec_dev_new(&out_cfg);
    assert(c->output_dev != NULL);
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = c->codec_if, .data_if = c->data_if };
    c->input_dev = esp_codec_dev_new(&in_cfg);
    assert(c->input_dev != NULL);
    esp_codec_set_disable_when_closed(c->output_dev, false);
    esp_codec_set_disable_when_closed(c->input_dev, false);
    ESP_LOGI(TAG, "ES8389 codec initialized");
    return &c->base;
}

audio_codec_t *box_codec_create(void *i2c_handle,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t es8311_addr, uint8_t es7210_addr,
    bool input_reference)
{
    i2c_audio_codec_t *c = codec_alloc(CODEC_BOX, in_rate, out_rate, input_reference,
                                      input_reference ? 2 : 1, 30.0f, (gpio_num_t)pa_pin);
    if (!c) return NULL;
    create_box_channels(c, (gpio_num_t)mclk, (gpio_num_t)bclk, (gpio_num_t)ws, (gpio_num_t)dout, (gpio_num_t)din);

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = c->base.rx_handle, .tx_handle = c->base.tx_handle };
    c->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(c->data_if != NULL);

    audio_codec_i2c_cfg_t i2c_cfg = { .port = (i2c_port_t)1, .addr = es8311_addr, .bus_handle = i2c_handle };
    c->out_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(c->out_ctrl_if != NULL);
    c->gpio_if = audio_codec_new_gpio();
    assert(c->gpio_if != NULL);

    es8311_codec_cfg_t es8311_cfg = {0};
    es8311_cfg.ctrl_if = c->out_ctrl_if;
    es8311_cfg.gpio_if = c->gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = (gpio_num_t)pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    c->out_codec_if = es8311_codec_new(&es8311_cfg);
    assert(c->out_codec_if != NULL);

    esp_codec_dev_cfg_t dev_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = c->out_codec_if, .data_if = c->data_if };
    c->output_dev = esp_codec_dev_new(&dev_cfg);
    assert(c->output_dev != NULL);

    i2c_cfg.addr = es7210_addr;
    c->in_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(c->in_ctrl_if != NULL);
    es7210_codec_cfg_t es7210_cfg = {0};
    es7210_cfg.ctrl_if = c->in_ctrl_if;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    c->in_codec_if = es7210_codec_new(&es7210_cfg);
    assert(c->in_codec_if != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = c->in_codec_if;
    c->input_dev = esp_codec_dev_new(&dev_cfg);
    assert(c->input_dev != NULL);
    ESP_LOGI(TAG, "Box codec initialized");
    return &c->base;
}
