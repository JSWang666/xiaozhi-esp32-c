#include "no_audio_codec.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <esp_log.h>
#include <driver/i2s_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define TAG "NoAudioCodec"

typedef struct {
    audio_codec_t base;
    SemaphoreHandle_t data_if_mutex;
} no_audio_codec_t;

static int no_audio_write(audio_codec_t *codec, const int16_t *data, int samples)
{
    no_audio_codec_t *impl = (no_audio_codec_t *)codec;
    xSemaphoreTake(impl->data_if_mutex, portMAX_DELAY);

    int32_t *buffer = (int32_t *)malloc(samples * sizeof(int32_t));
    if (!buffer) {
        xSemaphoreGive(impl->data_if_mutex);
        return 0;
    }

    int32_t volume_factor = (int32_t)(pow((double)codec->output_volume / 100.0, 2) * 65536);
    for (int i = 0; i < samples; i++) {
        int64_t temp = (int64_t)data[i] * volume_factor;
        if (temp > INT32_MAX) {
            buffer[i] = INT32_MAX;
        } else if (temp < INT32_MIN) {
            buffer[i] = INT32_MIN;
        } else {
            buffer[i] = (int32_t)temp;
        }
    }

    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(codec->tx_handle, buffer, samples * sizeof(int32_t), &bytes_written, portMAX_DELAY));
    free(buffer);
    xSemaphoreGive(impl->data_if_mutex);
    return (int)(bytes_written / sizeof(int32_t));
}

static int no_audio_read(audio_codec_t *codec, int16_t *dest, int samples)
{
    size_t bytes_read;
    const TickType_t read_timeout = pdMS_TO_TICKS(200);

    int32_t *bit32_buffer = (int32_t *)malloc(samples * sizeof(int32_t));
    if (!bit32_buffer) return 0;

    if (i2s_channel_read(codec->rx_handle, bit32_buffer, samples * sizeof(int32_t), &bytes_read, read_timeout) != ESP_OK) {
        free(bit32_buffer);
        return 0;
    }

    samples = (int)(bytes_read / sizeof(int32_t));
    for (int i = 0; i < samples; i++) {
        int32_t value = bit32_buffer[i] >> 12;
        dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : (int16_t)value;
    }
    free(bit32_buffer);
    return samples;
}

static void no_audio_enable_input(audio_codec_t *codec, bool enable)
{
    no_audio_codec_t *impl = (no_audio_codec_t *)codec;
    xSemaphoreTake(impl->data_if_mutex, portMAX_DELAY);
    if (enable == codec->input_enabled) {
        xSemaphoreGive(impl->data_if_mutex);
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(codec->rx_handle));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(codec->rx_handle));
    }
    audio_codec_base_enable_input(codec, enable);
    xSemaphoreGive(impl->data_if_mutex);
}

static void no_audio_enable_output(audio_codec_t *codec, bool enable)
{
    no_audio_codec_t *impl = (no_audio_codec_t *)codec;
    xSemaphoreTake(impl->data_if_mutex, portMAX_DELAY);
    if (enable == codec->output_enabled) {
        xSemaphoreGive(impl->data_if_mutex);
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(codec->tx_handle));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(codec->tx_handle));
    }
    audio_codec_base_enable_output(codec, enable);
    xSemaphoreGive(impl->data_if_mutex);
}

static void no_audio_destroy(audio_codec_t *codec)
{
    no_audio_codec_t *impl = (no_audio_codec_t *)codec;
    if (codec->rx_handle) {
        i2s_channel_disable(codec->rx_handle);
    }
    if (codec->tx_handle) {
        i2s_channel_disable(codec->tx_handle);
    }
    if (impl->data_if_mutex) {
        vSemaphoreDelete(impl->data_if_mutex);
    }
    free(impl);
}

static const audio_codec_ops_t no_audio_ops = {
    .read = no_audio_read,
    .write = no_audio_write,
    .set_output_volume = NULL,
    .set_input_gain = NULL,
    .enable_input = no_audio_enable_input,
    .enable_output = no_audio_enable_output,
    .start = NULL,
    .destroy = no_audio_destroy,
};

static no_audio_codec_t *no_audio_alloc(void)
{
    no_audio_codec_t *impl = (no_audio_codec_t *)calloc(1, sizeof(*impl));
    if (!impl) return NULL;

    audio_codec_base_init(&impl->base);
    impl->base.ops = &no_audio_ops;
    impl->data_if_mutex = xSemaphoreCreateMutex();
    if (!impl->data_if_mutex) {
        free(impl);
        return NULL;
    }
    return impl;
}

audio_codec_t *no_audio_codec_duplex_create(int input_sample_rate, int output_sample_rate,
                                             gpio_num_t bclk, gpio_num_t ws,
                                             gpio_num_t dout, gpio_num_t din)
{
    no_audio_codec_t *impl = no_audio_alloc();
    if (!impl) return NULL;

    impl->base.duplex = true;
    impl->base.input_sample_rate = input_sample_rate;
    impl->base.output_sample_rate = output_sample_rate;

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &impl->base.tx_handle, &impl->base.rx_handle));

    i2s_std_config_t std_cfg;
    memset(&std_cfg, 0, sizeof(std_cfg));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)output_sample_rate;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;
#ifdef I2S_HW_VERSION_2
    std_cfg.slot_cfg.left_align = true;
    std_cfg.slot_cfg.big_endian = false;
    std_cfg.slot_cfg.bit_order_lsb = false;
#endif
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = bclk;
    std_cfg.gpio_cfg.ws = ws;
    std_cfg.gpio_cfg.dout = dout;
    std_cfg.gpio_cfg.din = din;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(impl->base.tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(impl->base.rx_handle, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
    return &impl->base;
}

static void init_simplex_std_cfg(i2s_std_config_t *cfg, uint32_t sample_rate,
                                 i2s_std_slot_mask_t slot_mask,
                                 gpio_num_t bclk, gpio_num_t ws,
                                 gpio_num_t dout, gpio_num_t din)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->clk_cfg.sample_rate_hz = sample_rate;
    cfg->clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    cfg->clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg->slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    cfg->slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    cfg->slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    cfg->slot_cfg.slot_mask = slot_mask;
    cfg->slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    cfg->slot_cfg.ws_pol = false;
    cfg->slot_cfg.bit_shift = true;
#ifdef I2S_HW_VERSION_2
    cfg->slot_cfg.left_align = true;
    cfg->slot_cfg.big_endian = false;
    cfg->slot_cfg.bit_order_lsb = false;
#endif
    cfg->gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg->gpio_cfg.bclk = bclk;
    cfg->gpio_cfg.ws = ws;
    cfg->gpio_cfg.dout = dout;
    cfg->gpio_cfg.din = din;
}

audio_codec_t *no_audio_codec_simplex_create(int input_sample_rate, int output_sample_rate,
                                              gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                              gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din)
{
    return no_audio_codec_simplex_create_ex(input_sample_rate, output_sample_rate,
                                            spk_bclk, spk_ws, spk_dout, I2S_STD_SLOT_LEFT,
                                            mic_sck, mic_ws, mic_din, I2S_STD_SLOT_LEFT);
}

audio_codec_t *no_audio_codec_simplex_create_ex(int input_sample_rate, int output_sample_rate,
                                                 gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                                 i2s_std_slot_mask_t spk_slot_mask,
                                                 gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din,
                                                 i2s_std_slot_mask_t mic_slot_mask)
{
    no_audio_codec_t *impl = no_audio_alloc();
    if (!impl) return NULL;

    impl->base.duplex = false;
    impl->base.input_sample_rate = input_sample_rate;
    impl->base.output_sample_rate = output_sample_rate;

    i2s_chan_config_t chan_cfg = {
        .id = (i2s_port_t)0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &impl->base.tx_handle, NULL));

    i2s_std_config_t std_cfg;
    init_simplex_std_cfg(&std_cfg, (uint32_t)output_sample_rate, spk_slot_mask,
                         spk_bclk, spk_ws, spk_dout, I2S_GPIO_UNUSED);
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(impl->base.tx_handle, &std_cfg));

    chan_cfg.id = (i2s_port_t)1;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &impl->base.rx_handle));
    init_simplex_std_cfg(&std_cfg, (uint32_t)input_sample_rate, mic_slot_mask,
                         mic_sck, mic_ws, I2S_GPIO_UNUSED, mic_din);
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(impl->base.rx_handle, &std_cfg));
    ESP_LOGI(TAG, "Simplex channels created");
    return &impl->base;
}
