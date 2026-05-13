#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "audio/codecs/no_audio_codec.h"
#include "boards/common/backlight.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/spi_master.h>
#include <esp_lcd_gc9a01.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#define TAG "ElectronBot"

extern void InitializeElectronBotController(void);

typedef struct {
    board_desc_t base;
    display_t    *display;
    audio_codec_t *codec;
    backlight_t  *backlight;
    board_btn_t  *boot_button;
} electron_bot_ctx_t;

/* ---- button callback ---- */
static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting)
        return;
    app_toggle_chat(app);
}

/* ---- hardware init helpers ---- */
static void init_spi(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(
        DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
        DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_gc9a01_display(electron_bot_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init GC9A01 display");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config =
        GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, NULL, NULL);
    io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_SPI_RESET_PIN,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ctx->display = spi_lcd_display_create(io_handle, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT,
        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(electron_bot_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

/* ---- vtable functions ---- */
static const char *eb_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *eb_get_audio_codec(board_desc_t *self)
{
    electron_bot_ctx_t *ctx = (electron_bot_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = no_audio_codec_simplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
    }
    return ctx->codec;
}

static void *eb_get_display(board_desc_t *self)
{
    electron_bot_ctx_t *ctx = (electron_bot_ctx_t *)self;
    return ctx->display;
}

static void *eb_get_backlight(board_desc_t *self)
{
    electron_bot_ctx_t *ctx = (electron_bot_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(
            DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void eb_destroy(board_desc_t *self)
{
    electron_bot_ctx_t *ctx = (electron_bot_ctx_t *)self;
    if (ctx->boot_button)
        board_btn_delete(ctx->boot_button);
    free(ctx);
}

/* ---- public entry point ---- */
board_desc_t *create_board_desc(void)
{
    electron_bot_ctx_t *ctx = calloc(1, sizeof(electron_bot_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind             = BOARD_KIND_WIFI;
    ctx->base.get_board_type   = eb_get_board_type;
    ctx->base.get_audio_codec  = eb_get_audio_codec;
    ctx->base.get_display      = eb_get_display;
    ctx->base.get_backlight    = eb_get_backlight;
    ctx->base.destroy          = eb_destroy;

    init_spi();
    init_gc9a01_display(ctx);
    init_buttons(ctx);

    InitializeElectronBotController();

    if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
        eb_get_backlight(&ctx->base);
        backlight_restore_brightness(ctx->backlight);
    }

    return &ctx->base;
}
