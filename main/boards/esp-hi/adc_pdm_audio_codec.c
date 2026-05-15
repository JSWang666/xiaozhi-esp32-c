#include "audio_codec.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <driver/i2s_pdm.h>
#include <driver/gpio.h>
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "hal/gpio_ll.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>

static const char TAG[] = "AdcPdmAudioCodec";

#define TIMER_TIMEOUT_US 120000

typedef struct {
    audio_codec_t base;
    esp_codec_dev_handle_t output_dev;
    esp_codec_dev_handle_t input_dev;
    gpio_num_t pa_ctrl_pin;
    esp_timer_handle_t output_timer;
} adc_pdm_codec_t;

static void output_timer_callback(void *arg)
{
    adc_pdm_codec_t *c = (adc_pdm_codec_t *)arg;
    if (c->base.output_enabled) {
        if (c->base.ops && c->base.ops->enable_output)
            c->base.ops->enable_output(&c->base, false);
    }
}

static int apc_read(audio_codec_t *codec, int16_t *dest, int samples)
{
    adc_pdm_codec_t *c = (adc_pdm_codec_t *)codec;
    if (codec->input_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(c->input_dev, (void *)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

static int apc_write(audio_codec_t *codec, const int16_t *data, int samples)
{
    adc_pdm_codec_t *c = (adc_pdm_codec_t *)codec;
    if (codec->output_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(c->output_dev, (void *)data, samples * sizeof(int16_t)));
        if (c->output_timer) {
            esp_timer_stop(c->output_timer);
            esp_timer_start_once(c->output_timer, TIMER_TIMEOUT_US);
        }
    }
    return samples;
}

static void apc_set_output_volume(audio_codec_t *codec, int volume)
{
    adc_pdm_codec_t *c = (adc_pdm_codec_t *)codec;
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(c->output_dev, volume));
    audio_codec_base_set_output_volume(codec, volume);
}

static void apc_enable_input(audio_codec_t *codec, bool enable)
{
    adc_pdm_codec_t *c = (adc_pdm_codec_t *)codec;
    if (enable == codec->input_enabled) return;

    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)codec->input_sample_rate,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(c->input_dev, &fs));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(c->input_dev));
    }
    audio_codec_base_enable_input(codec, enable);
}

static void apc_enable_output(audio_codec_t *codec, bool enable)
{
    adc_pdm_codec_t *c = (adc_pdm_codec_t *)codec;
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

        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(codec->tx_handle));
        i2s_pdm_tx_clk_config_t clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG((uint32_t)codec->output_sample_rate);
        clk_cfg.up_sample_fs = AUDIO_PDM_UPSAMPLE_FS;
        ESP_ERROR_CHECK(i2s_channel_reconfig_pdm_tx_clock(codec->tx_handle, &clk_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(codec->tx_handle));

        if (c->pa_ctrl_pin != GPIO_NUM_NC)
            gpio_set_level(c->pa_ctrl_pin, 1);

        if (c->output_timer)
            esp_timer_start_once(c->output_timer, TIMER_TIMEOUT_US);
    } else {
        if (c->output_timer)
            esp_timer_stop(c->output_timer);

        if (c->pa_ctrl_pin != GPIO_NUM_NC)
            gpio_set_level(c->pa_ctrl_pin, 0);

        ESP_ERROR_CHECK(esp_codec_dev_close(c->output_dev));
    }
    audio_codec_base_enable_output(codec, enable);
}

static void apc_start(audio_codec_t *codec)
{
    apc_enable_input(codec, true);
    apc_enable_output(codec, true);
    ESP_LOGI(TAG, "Audio codec started");
}

static void apc_destroy(audio_codec_t *codec)
{
    adc_pdm_codec_t *c = (adc_pdm_codec_t *)codec;
    if (c->output_timer) {
        esp_timer_stop(c->output_timer);
        esp_timer_delete(c->output_timer);
    }
    if (c->output_dev) {
        esp_codec_dev_close(c->output_dev);
        esp_codec_dev_delete(c->output_dev);
    }
    if (c->input_dev) {
        esp_codec_dev_close(c->input_dev);
        esp_codec_dev_delete(c->input_dev);
    }
    audio_codec_destroy(codec);
    free(c);
}

static const audio_codec_ops_t adc_pdm_ops = {
    .read = apc_read,
    .write = apc_write,
    .set_output_volume = apc_set_output_volume,
    .enable_input = apc_enable_input,
    .enable_output = apc_enable_output,
    .start = apc_start,
    .destroy = apc_destroy,
};

audio_codec_t *adc_pdm_audio_codec_create(
    int input_sample_rate, int output_sample_rate,
    uint32_t adc_mic_channel, int pdm_speak_p, int pdm_speak_n, int pa_ctl)
{
    adc_pdm_codec_t *c = calloc(1, sizeof(adc_pdm_codec_t));
    if (!c) return NULL;

    c->base.ops = &adc_pdm_ops;
    c->base.input_sample_rate = input_sample_rate;
    c->base.output_sample_rate = output_sample_rate;
    c->base.output_volume = 100;
    c->pa_ctrl_pin = (gpio_num_t)pa_ctl;

    audio_codec_adc_cfg_t cfg = {};
    cfg.handle = NULL;
    cfg.continuous_cfg.max_store_buf_size = 1024 * 2;
    cfg.continuous_cfg.conv_frame_size = 1024;
    cfg.continuous_cfg.sample_freq_hz = (uint32_t)input_sample_rate;
    cfg.continuous_cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    cfg.continuous_cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    cfg.continuous_cfg.pattern_num = 1;
    cfg.continuous_cfg.cfg_mode = AUDIO_CODEC_ADC_CFG_MODE_SINGLE_UNIT;
    cfg.continuous_cfg.cfg.single_unit.unit_id = ADC_UNIT_1;
    cfg.continuous_cfg.cfg.single_unit.atten = ADC_ATTEN_DB_12;
    cfg.continuous_cfg.cfg.single_unit.bit_width = ADC_BITWIDTH_12;
    cfg.continuous_cfg.cfg.single_unit.channel_id[0] = (uint8_t)adc_mic_channel;
    const audio_codec_data_if_t *adc_if = audio_codec_new_adc_data(&cfg);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .data_if = adc_if,
    };
    c->input_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (!c->input_dev) {
        ESP_LOGE(TAG, "Failed to create input codec device");
        free(c);
        return NULL;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &c->base.tx_handle, NULL));

    i2s_pdm_tx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG((uint32_t)output_sample_rate),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = GPIO_NUM_NC,
            .dout = (gpio_num_t)pdm_speak_p,
            .invert_flags = { .clk_inv = false },
        },
    };
    pdm_cfg.clk_cfg.up_sample_fs = AUDIO_PDM_UPSAMPLE_FS;
    pdm_cfg.slot_cfg.sd_scale = I2S_PDM_SIG_SCALING_MUL_4;
    pdm_cfg.slot_cfg.hp_scale = I2S_PDM_SIG_SCALING_MUL_4;
    pdm_cfg.slot_cfg.lp_scale = I2S_PDM_SIG_SCALING_MUL_4;
    pdm_cfg.slot_cfg.sinc_scale = I2S_PDM_SIG_SCALING_MUL_4;

    ESP_ERROR_CHECK(i2s_channel_init_pdm_tx_mode(c->base.tx_handle, &pdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(c->base.tx_handle));

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = NULL,
        .tx_handle = c->base.tx_handle,
    };
    const audio_codec_data_if_t *i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);

    codec_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    codec_dev_cfg.codec_if = NULL;
    codec_dev_cfg.data_if = i2s_data_if;
    c->output_dev = esp_codec_dev_new(&codec_dev_cfg);

    if (pa_ctl != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << pa_ctl,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }
    gpio_set_drive_capability((gpio_num_t)pdm_speak_p, GPIO_DRIVE_CAP_0);

    if (pdm_speak_n != GPIO_NUM_NC) {
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pdm_speak_n], PIN_FUNC_GPIO);
        gpio_set_direction((gpio_num_t)pdm_speak_n, GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal((gpio_num_t)pdm_speak_n, I2SO_SD_OUT_IDX, 1, 0);
        gpio_set_drive_capability((gpio_num_t)pdm_speak_n, GPIO_DRIVE_CAP_0);
    }

    esp_timer_create_args_t timer_args = {
        .callback = output_timer_callback,
        .arg = c,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "output_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &c->output_timer));

    ESP_LOGI(TAG, "AdcPdmAudioCodec initialized");
    return &c->base;
}
