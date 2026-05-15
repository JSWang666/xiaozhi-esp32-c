#include "audio_codec.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>

#ifdef CONFIG_BOARD_TYPE_LILYGO_T_CAMERAPLUS_S3_V1_2
#include <driver/i2s_pdm.h>
#endif

static const char TAG[] = "Tcamerapluss3AudioCodec";

typedef struct {
    audio_codec_t base;
    uint32_t volume;
} tcamerapluss3_codec_t;

static int codec_read(audio_codec_t *codec, int16_t *dest, int samples)
{
    if (codec->input_enabled) {
        size_t bytes_read;
        i2s_channel_read(codec->rx_handle, dest, samples * sizeof(int16_t), &bytes_read, portMAX_DELAY);

        int16_t *ptr = dest;
        for (int i = 0; i < samples; i++) {
            int32_t amplified = *ptr * 20;
            *ptr++ = (amplified > 32767) ? 32767 : (amplified < -32768) ? -32768 : (int16_t)amplified;
        }
    }
    return samples;
}

static int codec_write(audio_codec_t *codec, const int16_t *data, int samples)
{
    tcamerapluss3_codec_t *ctx = (tcamerapluss3_codec_t *)codec;
    if (codec->output_enabled) {
        size_t bytes_written;
        int16_t *output_data = (int16_t *)malloc(samples * sizeof(int16_t));
        for (int i = 0; i < samples; i++) {
            output_data[i] = (int16_t)((float)data[i] * (float)(ctx->volume / 100.0f));
        }
        i2s_channel_write(codec->tx_handle, output_data, samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        free(output_data);
    }
    return samples;
}

static void codec_set_output_volume(audio_codec_t *codec, int volume)
{
    tcamerapluss3_codec_t *ctx = (tcamerapluss3_codec_t *)codec;
    ctx->volume = volume;
    audio_codec_base_set_output_volume(codec, volume);
}

static void codec_enable_input(audio_codec_t *codec, bool enable)
{
    audio_codec_base_enable_input(codec, enable);
}

static void codec_enable_output(audio_codec_t *codec, bool enable)
{
    audio_codec_base_enable_output(codec, enable);
}

static void codec_destroy(audio_codec_t *codec)
{
    audio_codec_destroy(codec);
    free(codec);
}

static const audio_codec_ops_t tcamerapluss3_ops = {
    .read = codec_read,
    .write = codec_write,
    .set_output_volume = codec_set_output_volume,
    .set_input_gain = NULL,
    .enable_input = codec_enable_input,
    .enable_output = codec_enable_output,
    .start = audio_codec_base_start,
    .destroy = codec_destroy,
};

audio_codec_t *tcamerapluss3_audio_codec_create(int input_sample_rate, int output_sample_rate,
    gpio_num_t mic_bclk, gpio_num_t mic_ws, gpio_num_t mic_data,
    gpio_num_t spkr_bclk, gpio_num_t spkr_lrclk, gpio_num_t spkr_data,
    bool input_reference)
{
    tcamerapluss3_codec_t *ctx = calloc(1, sizeof(tcamerapluss3_codec_t));
    if (!ctx) return NULL;

    audio_codec_t *codec = &ctx->base;
    codec->ops = &tcamerapluss3_ops;
    codec->duplex = true;
    codec->input_reference = input_reference;
    codec->input_channels = input_reference ? 2 : 1;
    codec->input_sample_rate = input_sample_rate;
    codec->output_sample_rate = output_sample_rate;
    ctx->volume = 70;

    i2s_chan_config_t mic_chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    mic_chan_config.auto_clear = true;
    i2s_chan_config_t spkr_chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    spkr_chan_config.auto_clear = true;

    ESP_ERROR_CHECK(i2s_new_channel(&mic_chan_config, NULL, &codec->rx_handle));
    ESP_ERROR_CHECK(i2s_new_channel(&spkr_chan_config, &codec->tx_handle, NULL));

#ifdef CONFIG_BOARD_TYPE_LILYGO_T_CAMERAPLUS_S3_V1_0_V1_1
    i2s_std_config_t mic_config = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = mic_bclk,
            .ws = mic_ws,
            .dout = I2S_GPIO_UNUSED,
            .din = mic_data,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = true,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(codec->rx_handle, &mic_config));
#elif defined CONFIG_BOARD_TYPE_LILYGO_T_CAMERAPLUS_S3_V1_2
    i2s_pdm_rx_config_t mic_config = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)input_sample_rate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = mic_ws,
            .din = mic_data,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(codec->rx_handle, &mic_config));
#endif

    i2s_std_config_t spkr_config = {
        .clk_cfg = {
            .sample_rate_hz = 11025,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spkr_bclk,
            .ws = spkr_lrclk,
            .dout = spkr_data,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(codec->tx_handle, &spkr_config));
    ESP_LOGI(TAG, "Voice hardware created");

#ifdef CONFIG_BOARD_TYPE_LILYGO_T_CAMERAPLUS_S3_V1_2
    gpio_config_t config_mic_spkr_en = {
        .pin_bit_mask = BIT64(AUDIO_MIC_SPKR_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
        .hys_ctrl_mode = GPIO_HYS_SOFT_ENABLE,
#endif
    };
    gpio_config(&config_mic_spkr_en);
    gpio_set_level(AUDIO_MIC_SPKR_EN, 0);
#endif

    audio_codec_base_init(codec);
    ESP_LOGI(TAG, "Tcamerapluss3AudioCodec initialized");
    return codec;
}
