#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "audio/codecs/no_audio_codec.h"
#include "led/single_led.h"
#include "backlight.h"

#include <stdlib.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>
#include <driver/spi_common.h>
#include "c_api/board_c_api.h"
#include "device_state.h"

#define TAG "eda_tv_pro"

typedef struct {
    board_desc_t base;
    display_t *display;
    audio_codec_t *codec;
    led_t *led;
    backlight_t *backlight;
    board_btn_t *boot_button;
} eda_tv_pro_ctx_t;

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

static void init_lcd_display(eda_tv_pro_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    ESP_LOGD(TAG, "Install panel IO");
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

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RST_PIN,
        .rgb_ele_order = DISPLAY_RGB_ORDER,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(eda_tv_pro_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *etp_get_board_type(board_desc_t *self)
{
    (void)self;
    return "eda-tv-pro";
}

static void *etp_get_led(board_desc_t *self)
{
    eda_tv_pro_ctx_t *ctx = (eda_tv_pro_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *etp_get_audio_codec(board_desc_t *self)
{
    eda_tv_pro_ctx_t *ctx = (eda_tv_pro_ctx_t *)self;
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

static void *etp_get_display(board_desc_t *self)
{
    eda_tv_pro_ctx_t *ctx = (eda_tv_pro_ctx_t *)self;
    return ctx->display;
}

static void *etp_get_backlight(board_desc_t *self)
{
    eda_tv_pro_ctx_t *ctx = (eda_tv_pro_ctx_t *)self;
    if (!ctx->backlight && DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void etp_destroy(board_desc_t *self)
{
    eda_tv_pro_ctx_t *ctx = (eda_tv_pro_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    eda_tv_pro_ctx_t *ctx = calloc(1, sizeof(eda_tv_pro_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = etp_get_board_type;
    ctx->base.get_led = etp_get_led;
    ctx->base.get_audio_codec = etp_get_audio_codec;
    ctx->base.get_display = etp_get_display;
    ctx->base.get_backlight = etp_get_backlight;
    ctx->base.destroy = etp_destroy;

    init_spi();
    init_lcd_display(ctx);
    init_buttons(ctx);

    if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
        backlight_t *bl = etp_get_backlight(&ctx->base);
        if (bl) backlight_restore_brightness(bl);
    }

    return &ctx->base;
}
