#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "led/single_led.h"
#include "backlight.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include "c_api/board_c_api.h"

#define TAG "Spotpear_ESP32_S3_1_28_BOX"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t codec_i2c_bus;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
} sp128_ctx_t;

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

static void init_codec_i2c(sp128_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->codec_i2c_bus));
}

static void init_spi(sp128_ctx_t *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
                                DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_gc9a01_display(sp128_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init GC9A01 display");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, 0, NULL);
    io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_SPI_RESET_PIN,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    uint8_t data_0x62[] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 };
    esp_lcd_panel_io_tx_param(io_handle, 0x62, data_0x62, sizeof(data_0x62));
    uint8_t data_0x63[] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 };
    esp_lcd_panel_io_tx_param(io_handle, 0x63, data_0x63, sizeof(data_0x63));
    uint8_t data_0x36[] = { 0x48 };
    esp_lcd_panel_io_tx_param(io_handle, 0x36, data_0x36, sizeof(data_0x36));
    uint8_t data_0xC3[] = { 0x1F };
    esp_lcd_panel_io_tx_param(io_handle, 0xC3, data_0xC3, sizeof(data_0xC3));
    uint8_t data_0xC4[] = { 0x1F };
    esp_lcd_panel_io_tx_param(io_handle, 0xC4, data_0xC4, sizeof(data_0xC4));

    ctx->display = spi_lcd_display_create(io_handle, panel_handle,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(sp128_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *get_board_type(board_desc_t *self) { (void)self; return BOARD_TYPE; }

static void *get_led(board_desc_t *self)
{
    sp128_ctx_t *ctx = (sp128_ctx_t *)self;
    if (!ctx->led) ctx->led = single_led_create(BUILTIN_LED_GPIO);
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    sp128_ctx_t *ctx = (sp128_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->codec_i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, false, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self) { return ((sp128_ctx_t *)self)->display; }

static void *get_backlight(board_desc_t *self)
{
    sp128_ctx_t *ctx = (sp128_ctx_t *)self;
    if (!ctx->backlight)
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    return ctx->backlight;
}

static void board_destroy(board_desc_t *self)
{
    sp128_ctx_t *ctx = (sp128_ctx_t *)self;
    if (ctx->backlight) {
        backlight_destroy(ctx->backlight);
        ctx->backlight = NULL;
    }
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    sp128_ctx_t *ctx = calloc(1, sizeof(sp128_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = board_destroy;

    init_codec_i2c(ctx);
    init_spi(ctx);
    init_gc9a01_display(ctx);
    init_buttons(ctx);

    backlight_t *bl = get_backlight(&ctx->base);
    if (bl && bl->ops && bl->ops->restore_brightness)
        bl->ops->restore_brightness(bl);

    return &ctx->base;
}
