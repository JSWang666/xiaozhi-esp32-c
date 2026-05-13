#include "board_defs.h"
#include "button.h"
#include "system_reset.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "backlight.h"
#include "assets/lang_c.h"
#include "device_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "GenJuTech_s3_1_54TFT"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t codec_i2c_bus;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
    board_btn_t *volume_up_button;
    board_btn_t *volume_down_button;
} genjutech_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void on_volume_up_click(void *ud)
{
    genjutech_ctx_t *ctx = (genjutech_ctx_t *)ud;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume + 10;
    if (vol > 100) vol = 100;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display)
        display_show_notification(ctx->display, buf, 0);
}

static void on_volume_up_long(void *ud)
{
    genjutech_ctx_t *ctx = (genjutech_ctx_t *)ud;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_max_volume, 0);
}

static void on_volume_down_click(void *ud)
{
    genjutech_ctx_t *ctx = (genjutech_ctx_t *)ud;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume - 10;
    if (vol < 0) vol = 0;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display)
        display_show_notification(ctx->display, buf, 0);
}

static void on_volume_down_long(void *ud)
{
    genjutech_ctx_t *ctx = (genjutech_ctx_t *)ud;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_muted, 0);
}

static void init_codec_i2c(genjutech_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->codec_i2c_bus));
}

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_SDA,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SCL,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_lcd_display(genjutech_ctx_t *ctx)
{
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << DISPLAY_RES),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    gpio_set_level(DISPLAY_RES, 0);
    vTaskDelay(20);
    gpio_set_level(DISPLAY_RES, 1);

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS,
        .dc_gpio_num = DISPLAY_DC,
        .spi_mode = 3,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RES,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(genjutech_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t vol_up_cfg = { .gpio_num = VOLUME_UP_BUTTON_GPIO };
    ctx->volume_up_button = board_btn_create_gpio(&vol_up_cfg);
    board_btn_on_click(ctx->volume_up_button, on_volume_up_click, ctx);
    board_btn_on_long_press(ctx->volume_up_button, on_volume_up_long, ctx);

    board_btn_gpio_cfg_t vol_down_cfg = { .gpio_num = VOLUME_DOWN_BUTTON_GPIO };
    ctx->volume_down_button = board_btn_create_gpio(&vol_down_cfg);
    board_btn_on_click(ctx->volume_down_button, on_volume_down_click, ctx);
    board_btn_on_long_press(ctx->volume_down_button, on_volume_down_long, ctx);
}

static const char *genjutech_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *genjutech_get_led(board_desc_t *self)
{
    genjutech_ctx_t *ctx = (genjutech_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *genjutech_get_audio_codec(board_desc_t *self)
{
    genjutech_ctx_t *ctx = (genjutech_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->codec_i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, false);
    }
    return ctx->codec;
}

static void *genjutech_get_display(board_desc_t *self)
{
    genjutech_ctx_t *ctx = (genjutech_ctx_t *)self;
    return ctx->display;
}

static void *genjutech_get_backlight(board_desc_t *self)
{
    genjutech_ctx_t *ctx = (genjutech_ctx_t *)self;
    return ctx->backlight;
}

static void genjutech_destroy(board_desc_t *self)
{
    genjutech_ctx_t *ctx = (genjutech_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->volume_up_button);
    board_btn_delete(ctx->volume_down_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    genjutech_ctx_t *ctx = calloc(1, sizeof(genjutech_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = genjutech_get_board_type;
    ctx->base.get_led = genjutech_get_led;
    ctx->base.get_audio_codec = genjutech_get_audio_codec;
    ctx->base.get_display = genjutech_get_display;
    ctx->base.get_backlight = genjutech_get_backlight;
    ctx->base.destroy = genjutech_destroy;

    init_codec_i2c(ctx);
    init_spi();
    init_lcd_display(ctx);
    init_buttons(ctx);

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
        DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    if (ctx->backlight) {
        backlight_restore_brightness(ctx->backlight);
    }

    return &ctx->base;
}
