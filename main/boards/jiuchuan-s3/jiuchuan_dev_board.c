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
#include "device_state.h"

#include <stdlib.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "esp_lcd_panel_gc9301.h"

#define TAG "JiuchuanDevBoard"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t codec_i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
} jiuchuan_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void init_i2c(jiuchuan_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = 1,
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

static void init_lcd_display(jiuchuan_ctx_t *ctx)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_SPI_MOSI_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SPI_SCK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_SPI_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = 3,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &ctx->panel_io);

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_new_panel_gc9309na(ctx->panel_io, &panel_config, &ctx->panel);

    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, false);
    esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(jiuchuan_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *jiuchuan_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *jiuchuan_get_led(board_desc_t *self)
{
    jiuchuan_ctx_t *ctx = (jiuchuan_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *jiuchuan_get_audio_codec(board_desc_t *self)
{
    jiuchuan_ctx_t *ctx = (jiuchuan_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->codec_i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, false);
    }
    return ctx->codec;
}

static void *jiuchuan_get_display(board_desc_t *self)
{
    jiuchuan_ctx_t *ctx = (jiuchuan_ctx_t *)self;
    return ctx->display;
}

static void *jiuchuan_get_backlight(board_desc_t *self)
{
    jiuchuan_ctx_t *ctx = (jiuchuan_ctx_t *)self;
    return ctx->backlight;
}

static void jiuchuan_destroy(board_desc_t *self)
{
    jiuchuan_ctx_t *ctx = (jiuchuan_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    jiuchuan_ctx_t *ctx = calloc(1, sizeof(jiuchuan_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = jiuchuan_get_board_type;
    ctx->base.get_led = jiuchuan_get_led;
    ctx->base.get_audio_codec = jiuchuan_get_audio_codec;
    ctx->base.get_display = jiuchuan_get_display;
    ctx->base.get_backlight = jiuchuan_get_backlight;
    ctx->base.destroy = jiuchuan_destroy;

    init_i2c(ctx);
    init_lcd_display(ctx);
    init_buttons(ctx);

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
        DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    if (ctx->backlight) {
        backlight_restore_brightness(ctx->backlight);
    }

    return &ctx->base;
}
