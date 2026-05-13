#include "board_defs.h"
#include "button.h"
#include "system_reset.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/board_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "audio/codecs/no_audio_codec.h"
#include "backlight.h"
#include "device_state.h"

#include <stdlib.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
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

#define TAG "CompactWifiBoardS3Cam"

typedef struct {
    board_desc_t base;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
} s3cam_ctx_t;

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

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_CLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_lcd_display(s3cam_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = DISPLAY_SPI_MODE,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RST_PIN,
        .rgb_ele_order = DISPLAY_RGB_ORDER,
        .bits_per_pixel = 16,
    };

#if defined(LCD_TYPE_ILI9341_SERIAL)
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
    gc9a01_vendor_config_t gc9107_vendor_config = {
        .init_cmds = gc9107_lcd_init_cmds,
        .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
    };
    panel_config.vendor_config = &gc9107_vendor_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
#else
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(s3cam_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *s3cam_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *s3cam_get_led(board_desc_t *self)
{
    s3cam_ctx_t *ctx = (s3cam_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *s3cam_get_audio_codec(board_desc_t *self)
{
    s3cam_ctx_t *ctx = (s3cam_ctx_t *)self;
    if (!ctx->codec) {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        ctx->codec = no_audio_codec_simplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        ctx->codec = no_audio_codec_duplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
    }
    return ctx->codec;
}

static void *s3cam_get_display(board_desc_t *self)
{
    s3cam_ctx_t *ctx = (s3cam_ctx_t *)self;
    return ctx->display;
}

static void *s3cam_get_backlight(board_desc_t *self)
{
    s3cam_ctx_t *ctx = (s3cam_ctx_t *)self;
    return ctx->backlight;
}

static void s3cam_destroy(board_desc_t *self)
{
    s3cam_ctx_t *ctx = (s3cam_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    s3cam_ctx_t *ctx = calloc(1, sizeof(s3cam_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = s3cam_get_board_type;
    ctx->base.get_led = s3cam_get_led;
    ctx->base.get_audio_codec = s3cam_get_audio_codec;
    ctx->base.get_display = s3cam_get_display;
    ctx->base.get_backlight = s3cam_get_backlight;
    ctx->base.destroy = s3cam_destroy;

    init_spi();
    init_lcd_display(ctx);
    init_buttons(ctx);

    if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
        if (ctx->backlight) {
            backlight_restore_brightness(ctx->backlight);
        }
    }

    return &ctx->base;
}
