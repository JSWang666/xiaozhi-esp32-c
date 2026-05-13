#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "device_state.h"

typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

display_t *custom_rlcd_display_create(const spi_display_config_t *cfg, int width, int height);

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

#define TAG "waveshare_rlcd_4_2"

#define BOARD_TYPE "WaveshareEsp32s3RLCD4inch2"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    audio_codec_t *codec;
    display_t *display;

    board_btn_t *boot_button;
} rlcd42_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void init_i2c(rlcd42_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = ESP32_I2C_HOST,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->i2c_bus));
}

static void init_buttons(rlcd42_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static void init_display(rlcd42_ctx_t *ctx)
{
    spi_display_config_t spi_cfg = {
        .mosi = RLCD_MOSI_PIN,
        .scl = RLCD_SCK_PIN,
        .dc = RLCD_DC_PIN,
        .cs = RLCD_CS_PIN,
        .rst = RLCD_RST_PIN,
    };
    ctx->display = custom_rlcd_display_create(&spi_cfg, RLCD_WIDTH, RLCD_HEIGHT);
}

static uint16_t battery_get_voltage(void)
{
    static bool initialized = false;
    static adc_oneshot_unit_handle_t adc_handle;
    static adc_cali_handle_t cali_handle = NULL;
    if (!initialized) {
        adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };
        adc_oneshot_new_unit(&init_config, &adc_handle);

        adc_oneshot_chan_cfg_t ch_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config);

        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
            initialized = true;
        }
    }

    if (initialized) {
        int raw_value = 0;
        int raw_voltage = 0;
        adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
        adc_cali_raw_to_voltage(cali_handle, raw_value, &raw_voltage);
        return (uint16_t)(raw_voltage * 3);
    }
    return 0;
}

static uint8_t battery_get_percent(void)
{
    int voltage = 0;
    for (int i = 0; i < 10; i++) {
        voltage += battery_get_voltage();
    }
    voltage /= 10;
    int percent = (-1 * voltage * voltage + 9016 * voltage - 19189000) / 10000;
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    return (uint8_t)percent;
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_audio_codec(board_desc_t *self)
{
    rlcd42_ctx_t *ctx = (rlcd42_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = box_codec_create(ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    rlcd42_ctx_t *ctx = (rlcd42_ctx_t *)self;
    return ctx->display;
}

static bool get_battery_level(board_desc_t *self, int *level, bool *charging, bool *discharging)
{
    (void)self;
    *charging = false;
    *discharging = true;
    *level = (int)battery_get_percent();
    return true;
}

static void board_destroy(board_desc_t *self)
{
    rlcd42_ctx_t *ctx = (rlcd42_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    rlcd42_ctx_t *ctx = calloc(1, sizeof(rlcd42_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_battery_level = get_battery_level;
    ctx->base.destroy = board_destroy;

    init_i2c(ctx);
    init_buttons(ctx);
    init_display(ctx);

    return &ctx->base;
}
