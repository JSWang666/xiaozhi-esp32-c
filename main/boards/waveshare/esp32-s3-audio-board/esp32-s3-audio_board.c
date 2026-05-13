#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "backlight.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>
#include "esp_io_expander_tca95xx_16bit.h"
#ifdef LCD_TYPE_JD9853_SERIAL
#include "esp_lcd_jd9853.h"
#include "c_api/board_c_api.h"
#endif

#define TAG "waveshare_s3_audio_board"

led_t *circular_strip_led_create(int gpio, int max_leds);

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_io_expander_handle_t io_expander;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
} wsab_ctx_t;

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

static void init_i2c(wsab_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->i2c_bus));
}

static void init_tca9555(wsab_ctx_t *ctx)
{
    esp_err_t ret = esp_io_expander_new_i2c_tca95xx_16bit(ctx->i2c_bus, I2C_ADDRESS, &ctx->io_expander);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "TCA9555 create returned error");

    ret = esp_io_expander_set_dir(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 |
        IO_EXPANDER_PIN_NUM_8 | IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_6,
        IO_EXPANDER_OUTPUT);
    ESP_ERROR_CHECK(ret);
    ret = esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);
    ESP_ERROR_CHECK(ret);
    vTaskDelay(pdMS_TO_TICKS(10));
    ret = esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 0);
    ESP_ERROR_CHECK(ret);
    vTaskDelay(pdMS_TO_TICKS(10));
    ret = esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);
    ESP_ERROR_CHECK(ret);
    esp_io_expander_set_level(ctx->io_expander, IO_EXPANDER_PIN_NUM_8, 1);
    esp_io_expander_set_level(ctx->io_expander, IO_EXPANDER_PIN_NUM_5, false);
    vTaskDelay(pdMS_TO_TICKS(5));
    esp_io_expander_set_level(ctx->io_expander, IO_EXPANDER_PIN_NUM_6, true);
    vTaskDelay(pdMS_TO_TICKS(5));
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
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

#ifndef LCD_TYPE_JD9853_SERIAL
static void init_st7789_display(wsab_ctx_t *ctx)
{
    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_SCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &ctx->panel_io));

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(ctx->panel, DISPLAY_INVERT_COLOR));

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}
#endif

#ifdef LCD_TYPE_JD9853_SERIAL
static void init_jd9853_display(wsab_ctx_t *ctx)
{
    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_SCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &ctx->panel_io));

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(ctx->panel_io, &panel_config, &ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(ctx->panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(ctx->panel, 0, 34));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(ctx->panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(ctx->panel, true));

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}
#endif

static void init_buttons(wsab_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_led(board_desc_t *self)
{
    wsab_ctx_t *ctx = (wsab_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = circular_strip_led_create(BUILTIN_LED_GPIO, 6);
    }
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    wsab_ctx_t *ctx = (wsab_ctx_t *)self;
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
    wsab_ctx_t *ctx = (wsab_ctx_t *)self;
    return ctx->display;
}

static void *get_backlight(board_desc_t *self)
{
    wsab_ctx_t *ctx = (wsab_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(
            DISPLAY_BACKLIGHT_PIN, BACKLIGHT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void destroy(board_desc_t *self)
{
    wsab_ctx_t *ctx = (wsab_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    wsab_ctx_t *ctx = calloc(1, sizeof(wsab_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    init_i2c(ctx);
    init_tca9555(ctx);
    init_spi();
    init_buttons(ctx);

#ifdef LCD_TYPE_JD9853_SERIAL
    init_jd9853_display(ctx);
#else
    init_st7789_display(ctx);
#endif

    backlight_restore_brightness(get_backlight(&ctx->base));

    return &ctx->base;
}
