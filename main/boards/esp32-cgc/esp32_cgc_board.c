#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/mcp_server_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "audio/codecs/no_audio_codec.h"
#include "backlight.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include <esp_lcd_ili9341.h>
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include <esp_lcd_gc9a01.h>
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
     (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                 0x04, 0x12, 0x14, 0x1f},
     14, 0},
    {0xf1,
     (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                 0x0C, 0x1A, 0x14, 0x1E},
     14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif

#ifndef DISPLAY_SPI_MODE
#define DISPLAY_SPI_MODE 0
#endif

#define TAG "ESP32_CGC"

typedef struct {
    board_desc_t base;

    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
    board_btn_t *asr_button;

    bool lamp_power;
    gpio_num_t lamp_gpio;
} cgc_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void on_asr_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_wake_word_invoke(app, "\xe4\xbd\xa0\xe5\xa5\xbd\xe5\xb0\x8f\xe6\x99\xba");
}

static mcp_tool_result_t lamp_get_state(const void *args, void *ud)
{
    cgc_ctx_t *ctx = (cgc_ctx_t *)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    res.text = strdup(ctx->lamp_power ? "{\"power\": true}" : "{\"power\": false}");
    return res;
}

static mcp_tool_result_t lamp_turn_on(const void *args, void *ud)
{
    cgc_ctx_t *ctx = (cgc_ctx_t *)ud;
    ctx->lamp_power = true;
    gpio_set_level(ctx->lamp_gpio, 1);
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t lamp_turn_off(const void *args, void *ud)
{
    cgc_ctx_t *ctx = (cgc_ctx_t *)ud;
    ctx->lamp_power = false;
    gpio_set_level(ctx->lamp_gpio, 0);
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SCLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_lcd_display(cgc_ctx_t *ctx)
{
    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = DISPLAY_SPI_MODE,
        .pclk_hz = DISPLAY_SPI_SCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &ctx->panel_io));

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RESET_PIN,
        .rgb_ele_order = DISPLAY_RGB_ORDER,
        .bits_per_pixel = 16,
    };

#if defined(LCD_TYPE_ILI9341_SERIAL)
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(ctx->panel_io, &panel_config, &ctx->panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(ctx->panel_io, &panel_config, &ctx->panel));
#else
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel));
#endif

    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(cgc_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t asr_cfg = { .gpio_num = ASR_BUTTON_GPIO };
    ctx->asr_button = board_btn_create_gpio(&asr_cfg);
    board_btn_on_click(ctx->asr_button, on_asr_click, ctx);
}

static void init_lamp(cgc_ctx_t *ctx)
{
    ctx->lamp_gpio = LAMP_GPIO;
    if (ctx->lamp_gpio == GPIO_NUM_NC) return;

    gpio_config_t config = {
        .pin_bit_mask = (1ULL << ctx->lamp_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_set_level(ctx->lamp_gpio, 0);

    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return;

    mcp_server_add_tool_c(mcp, "self.lamp.get_state",
        "Get the power state of the lamp", NULL, 0, lamp_get_state, ctx);
    mcp_server_add_tool_c(mcp, "self.lamp.turn_on",
        "Turn on the lamp", NULL, 0, lamp_turn_on, ctx);
    mcp_server_add_tool_c(mcp, "self.lamp.turn_off",
        "Turn off the lamp", NULL, 0, lamp_turn_off, ctx);
}

static const char *cgc_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *cgc_get_led(board_desc_t *self)
{
    (void)self;
    return NULL;
}

static void *cgc_get_audio_codec(board_desc_t *self)
{
    cgc_ctx_t *ctx = (cgc_ctx_t *)self;
    if (!ctx->codec) {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        ctx->codec = no_audio_codec_simplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        ctx->codec = no_audio_codec_duplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
    }
    return ctx->codec;
}

static void *cgc_get_display(board_desc_t *self)
{
    cgc_ctx_t *ctx = (cgc_ctx_t *)self;
    return ctx->display;
}

static void *cgc_get_backlight(board_desc_t *self)
{
    cgc_ctx_t *ctx = (cgc_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void cgc_destroy(board_desc_t *self)
{
    cgc_ctx_t *ctx = (cgc_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->asr_button);
    if (ctx->backlight) backlight_destroy(ctx->backlight);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    cgc_ctx_t *ctx = calloc(1, sizeof(cgc_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = cgc_get_board_type;
    ctx->base.get_led = cgc_get_led;
    ctx->base.get_audio_codec = cgc_get_audio_codec;
    ctx->base.get_display = cgc_get_display;
    ctx->base.get_backlight = cgc_get_backlight;
    ctx->base.destroy = cgc_destroy;

    init_spi();
    init_lcd_display(ctx);
    init_buttons(ctx);
    init_lamp(ctx);

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
        DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    backlight_restore_brightness(ctx->backlight);

    return &ctx->base;
}
