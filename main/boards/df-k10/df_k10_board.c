#include "board_defs.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "assets/lang_c.h"
#include "device_state.h"
#include "display/display.h"
#include "audio/audio_codec.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <led_strip.h>
#include <iot_button.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_lcd_ili9341.h"
#include "esp_io_expander_tca95xx_16bit.h"

#define TAG "DF-K10"

audio_codec_t *k10_audio_codec_create(i2c_master_bus_handle_t i2c_bus);
led_strip_handle_t k10_led_control_init(void);

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_io_expander_handle_t io_expander;
    display_t *display;
    audio_codec_t *codec;
    led_strip_handle_t led_strip;

    button_handle_t btn_a;
    button_handle_t btn_b;
    button_driver_t *btn_a_driver;
    button_driver_t *btn_b_driver;
} df_k10_ctx_t;

static df_k10_ctx_t *s_instance = NULL;

static void init_i2c(df_k10_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = (i2c_port_t)1,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->i2c_bus));
}

static uint8_t io_expander_get_level(df_k10_ctx_t *ctx, uint16_t pin_mask)
{
    uint32_t pin_val = 0;
    esp_io_expander_get_level(ctx->io_expander, DRV_IO_EXP_INPUT_MASK, &pin_val);
    pin_mask &= DRV_IO_EXP_INPUT_MASK;
    return (uint8_t)((pin_val & pin_mask) ? 1 : 0);
}

static void init_io_expander(df_k10_ctx_t *ctx)
{
    esp_io_expander_new_i2c_tca95xx_16bit(
        ctx->i2c_bus, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &ctx->io_expander);

    esp_io_expander_set_dir(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);
    esp_io_expander_set_dir(ctx->io_expander, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);
}

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = GPIO_NUM_21,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = GPIO_NUM_12,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_display(df_k10_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_NUM_14,
        .dc_gpio_num = GPIO_NUM_13,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .bits_per_pixel = 16,
        .color_space = ESP_LCD_COLOR_SPACE_BGR,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static uint8_t btn_a_get_key_level(button_driver_t *drv)
{
    return !io_expander_get_level(s_instance, IO_EXPANDER_PIN_NUM_2);
}

static uint8_t btn_b_get_key_level(button_driver_t *drv)
{
    return !io_expander_get_level(s_instance, IO_EXPANDER_PIN_NUM_12);
}

static void btn_a_click_cb(void *button_handle, void *usr_data)
{
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void btn_a_long_press_cb(void *button_handle, void *usr_data)
{
    df_k10_ctx_t *ctx = (df_k10_ctx_t *)usr_data;
    if (!ctx->codec) return;
    int volume = ctx->codec->output_volume - 10;
    if (volume < 0) volume = 0;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, volume);
}

static void btn_b_click_cb(void *button_handle, void *usr_data)
{
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void btn_b_long_press_cb(void *button_handle, void *usr_data)
{
    df_k10_ctx_t *ctx = (df_k10_ctx_t *)usr_data;
    if (!ctx->codec) return;
    int volume = ctx->codec->output_volume + 10;
    if (volume > 100) volume = 100;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, volume);
}

static void init_buttons(df_k10_ctx_t *ctx)
{
    button_config_t btn_a_config = { .long_press_time = 1000, .short_press_time = 0 };
    ctx->btn_a_driver = calloc(1, sizeof(button_driver_t));
    ctx->btn_a_driver->enable_power_save = false;
    ctx->btn_a_driver->get_key_level = btn_a_get_key_level;
    ESP_ERROR_CHECK(iot_button_create(&btn_a_config, ctx->btn_a_driver, &ctx->btn_a));
    iot_button_register_cb(ctx->btn_a, BUTTON_SINGLE_CLICK, NULL, btn_a_click_cb, ctx);
    iot_button_register_cb(ctx->btn_a, BUTTON_LONG_PRESS_START, NULL, btn_a_long_press_cb, ctx);

    button_config_t btn_b_config = { .long_press_time = 1000, .short_press_time = 0 };
    ctx->btn_b_driver = calloc(1, sizeof(button_driver_t));
    ctx->btn_b_driver->enable_power_save = false;
    ctx->btn_b_driver->get_key_level = btn_b_get_key_level;
    ESP_ERROR_CHECK(iot_button_create(&btn_b_config, ctx->btn_b_driver, &ctx->btn_b));
    iot_button_register_cb(ctx->btn_b, BUTTON_SINGLE_CLICK, NULL, btn_b_click_cb, ctx);
    iot_button_register_cb(ctx->btn_b, BUTTON_LONG_PRESS_START, NULL, btn_b_long_press_cb, ctx);
}

static const char *k10_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *k10_get_audio_codec(board_desc_t *self)
{
    df_k10_ctx_t *ctx = (df_k10_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = k10_audio_codec_create(ctx->i2c_bus);
    }
    return ctx->codec;
}

static void *k10_get_display(board_desc_t *self)
{
    df_k10_ctx_t *ctx = (df_k10_ctx_t *)self;
    return ctx->display;
}

static void *k10_get_led(board_desc_t *self)
{
    df_k10_ctx_t *ctx = (df_k10_ctx_t *)self;
    return ctx->led_strip;
}

static void k10_destroy(board_desc_t *self)
{
    df_k10_ctx_t *ctx = (df_k10_ctx_t *)self;
    if (ctx->btn_a_driver) free(ctx->btn_a_driver);
    if (ctx->btn_b_driver) free(ctx->btn_b_driver);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    df_k10_ctx_t *ctx = calloc(1, sizeof(df_k10_ctx_t));
    if (!ctx) return NULL;
    s_instance = ctx;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = k10_get_board_type;
    ctx->base.get_audio_codec = k10_get_audio_codec;
    ctx->base.get_display = k10_get_display;
    ctx->base.get_led = k10_get_led;
    ctx->base.destroy = k10_destroy;

    init_i2c(ctx);
    init_io_expander(ctx);
    init_spi();
    init_display(ctx);
    init_buttons(ctx);

    ctx->led_strip = k10_led_control_init();

    return &ctx->base;
}
