#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/codec_c_api.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "led/single_led.h"
#include "assets/lang_c.h"
#include "device_state.h"
#include "backlight.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include "c_api/board_c_api.h"

#define TAG "XINGZHI_METAL_1_54_WIFI"

void cst816x_init(i2c_master_bus_handle_t i2c_bus, uint8_t addr,
                  audio_codec_t *codec, display_t *display, backlight_t *backlight);

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    bool touch_device_found;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
} xingzhi_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) {
        board_enter_wifi_config_mode(board_get_instance());
        return;
    }
    app_toggle_chat(app);
}

static void init_i2c(xingzhi_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->i2c_bus));

    ctx->touch_device_found = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
        if (i2c_master_probe(ctx->i2c_bus, addr, 100) == ESP_OK) {
            ESP_LOGI(TAG, "Device found at address 0x%02X", addr);
            if (addr == 0x15) {
                ctx->touch_device_found = true;
            }
        }
    }
}

static void init_spi(xingzhi_ctx_t *ctx)
{
    (void)ctx;
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_SDA,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SCL,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_st7789_display(xingzhi_ctx_t *ctx)
{
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
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

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

static void init_buttons(xingzhi_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *xz_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *xz_get_led(board_desc_t *self)
{
    (void)self;
    return NULL;
}

static void *xz_get_audio_codec(board_desc_t *self)
{
    xingzhi_ctx_t *ctx = (xingzhi_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(
            ctx->i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_I2C_PA_EN, AUDIO_CODEC_ES8311_ADDR,
            false, false);
    }
    return ctx->codec;
}

static void *xz_get_display(board_desc_t *self)
{
    xingzhi_ctx_t *ctx = (xingzhi_ctx_t *)self;
    return ctx->display;
}

static void *xz_get_backlight(board_desc_t *self)
{
    xingzhi_ctx_t *ctx = (xingzhi_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void xz_destroy(board_desc_t *self)
{
    xingzhi_ctx_t *ctx = (xingzhi_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    xingzhi_ctx_t *ctx = calloc(1, sizeof(xingzhi_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = xz_get_board_type;
    ctx->base.get_led = xz_get_led;
    ctx->base.get_audio_codec = xz_get_audio_codec;
    ctx->base.get_display = xz_get_display;
    ctx->base.get_backlight = xz_get_backlight;
    ctx->base.destroy = xz_destroy;

    init_i2c(ctx);
    init_spi(ctx);
    init_st7789_display(ctx);
    init_buttons(ctx);

    if (ctx->touch_device_found) {
        audio_codec_t *codec = xz_get_audio_codec(&ctx->base);
        backlight_t *bl = xz_get_backlight(&ctx->base);
        cst816x_init(ctx->i2c_bus, 0x15, codec, ctx->display, bl);
    }

    backlight_restore_brightness(xz_get_backlight(&ctx->base));

    return &ctx->base;
}
