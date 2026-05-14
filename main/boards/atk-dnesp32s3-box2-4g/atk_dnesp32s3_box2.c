#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "backlight.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "led/single_led.h"
#include "assets/lang_c.h"
#include "device_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include "esp_io_expander_tca95xx_16bit.h"

#define TAG "atk_dnesp32s3_box2_4g"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_io_expander_handle_t io_exp_handle;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *r_button;
} atk_dnesp32s3_box2_4g_ctx_t;

static atk_dnesp32s3_box2_4g_ctx_t *s_instance;

static uint8_t io_expander_get_level(atk_dnesp32s3_box2_4g_ctx_t *ctx, uint16_t pin_mask)
{
    uint32_t pin_val = 0;
    esp_io_expander_get_level(ctx->io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
    pin_mask &= DRV_IO_EXP_INPUT_MASK;
    return (uint8_t)((pin_val & pin_mask) ? 1 : 0);
}

static void on_volume_down(void *button_handle, void *usr_data)
{
    atk_dnesp32s3_box2_4g_ctx_t *ctx = (atk_dnesp32s3_box2_4g_ctx_t *)usr_data;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume - 10;
    if (vol < 0) vol = 0;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display) display_show_notification(ctx->display, buf, 0);
}

static void on_volume_down_long(void *button_handle, void *usr_data)
{
    atk_dnesp32s3_box2_4g_ctx_t *ctx = (atk_dnesp32s3_box2_4g_ctx_t *)usr_data;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
    if (ctx->display) display_show_notification(ctx->display, lang_str_muted, 0);
}

static void on_middle_click(void *button_handle, void *usr_data)
{
    (void)usr_data;
    app_context_t *app = app_get_context();
    if (!app) return;
    app_toggle_chat(app);
}

static void on_volume_up(void *button_handle, void *usr_data)
{
    atk_dnesp32s3_box2_4g_ctx_t *ctx = (atk_dnesp32s3_box2_4g_ctx_t *)usr_data;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume + 10;
    if (vol > 100) vol = 100;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display) display_show_notification(ctx->display, buf, 0);
}

static void on_volume_up_long(void *button_handle, void *usr_data)
{
    atk_dnesp32s3_box2_4g_ctx_t *ctx = (atk_dnesp32s3_box2_4g_ctx_t *)usr_data;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
    if (ctx->display) display_show_notification(ctx->display, lang_str_max_volume, 0);
}

static uint8_t xio_l_get_key_level(button_driver_t *drv)
{
    return !io_expander_get_level(s_instance, XIO_KEY_L);
}

static uint8_t xio_m_get_key_level(button_driver_t *drv)
{
    return io_expander_get_level(s_instance, XIO_KEY_M);
}

static void init_i2c(atk_dnesp32s3_box2_4g_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
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

static void init_io_expander(atk_dnesp32s3_box2_4g_ctx_t *ctx)
{
    esp_io_expander_new_i2c_tca95xx_16bit(ctx->i2c_bus, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &ctx->io_exp_handle);

    esp_io_expander_set_dir(ctx->io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_dir(ctx->io_exp_handle, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);

    esp_io_expander_set_level(ctx->io_exp_handle, XIO_SYS_POW, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_EN_3V3A, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_EN_4G, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_SPK_EN, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_USB_SEL, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_VBUS_EN, 0);
}

static void init_display(atk_dnesp32s3_box2_4g_ctx_t *ctx)
{
    gpio_config_t gpio_init_struct = {
        .pin_bit_mask = 1ull << LCD_PIN_RD,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_init_struct);
    gpio_set_level(LCD_PIN_RD, 1);

    gpio_init_struct.pin_bit_mask = 1ull << DISPLAY_BACKLIGHT_PIN;
    gpio_config(&gpio_init_struct);

    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .wr_gpio_num = LCD_PIN_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {
            LCD_PIN_D0, LCD_PIN_D1, LCD_PIN_D2, LCD_PIN_D3,
            LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 7,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 1,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &ctx->panel_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel));

    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, true);
    esp_lcd_panel_set_gap(ctx->panel, 0, 0);

    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xCF, (uint8_t[]){0x00,0x83,0x30}, 3);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xED, (uint8_t[]){0x64,0x03,0x12,0x81}, 4);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xE8, (uint8_t[]){0x85,0x01,0x79}, 3);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xCB, (uint8_t[]){0x39,0x2C,0x00,0x34,0x02}, 5);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xF7, (uint8_t[]){0x20}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xEA, (uint8_t[]){0x00,0x00}, 2);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xbb, (uint8_t[]){0x20}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xc3, (uint8_t[]){0x00}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xC4, (uint8_t[]){0x20}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xC5, (uint8_t[]){0x20}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xC6, (uint8_t[]){0x10}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xC7, (uint8_t[]){0xB0}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0x36, (uint8_t[]){0x60}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0x3A, (uint8_t[]){0x55}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xB1, (uint8_t[]){0x00,0x1B}, 2);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xF2, (uint8_t[]){0x08}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0x26, (uint8_t[]){0x01}, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xE0, (uint8_t[]){0xD0,0x00,0x02,0x07,0x0A,0x28,0x32,0x44,0x42,0x06,0x0E,0x12,0x14,0x17}, 14);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xE1, (uint8_t[]){0xD0,0x00,0x02,0x07,0x0A,0x28,0x31,0x54,0x47,0x0E,0x1C,0x17,0x1B,0x1E}, 14);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0xB7, (uint8_t[]){0x07}, 1);
    esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(atk_dnesp32s3_box2_4g_ctx_t *ctx)
{
    s_instance = ctx;

    button_config_t l_btn_cfg = { .long_press_time = 800, .short_press_time = 500 };
    button_config_t m_btn_cfg = { .long_press_time = 800, .short_press_time = 500 };
    button_config_t r_btn_cfg = { .long_press_time = 800, .short_press_time = 500 };

    button_handle_t l_btn_handle = NULL;
    button_handle_t m_btn_handle = NULL;
    button_handle_t r_btn_handle = NULL;

    button_driver_t *xio_l_btn_driver = (button_driver_t *)calloc(1, sizeof(button_driver_t));
    xio_l_btn_driver->enable_power_save = false;
    xio_l_btn_driver->get_key_level = xio_l_get_key_level;
    ESP_ERROR_CHECK(iot_button_create(&l_btn_cfg, xio_l_btn_driver, &l_btn_handle));

    button_driver_t *xio_m_btn_driver = (button_driver_t *)calloc(1, sizeof(button_driver_t));
    xio_m_btn_driver->enable_power_save = false;
    xio_m_btn_driver->get_key_level = xio_m_get_key_level;
    ESP_ERROR_CHECK(iot_button_create(&m_btn_cfg, xio_m_btn_driver, &m_btn_handle));

    button_gpio_config_t r_cfg = {
        .gpio_num = R_BUTTON_GPIO,
        .active_level = BUTTON_INACTIVE,
        .enable_power_save = false,
        .disable_pull = false,
    };
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&r_btn_cfg, &r_cfg, &r_btn_handle));

    iot_button_register_cb(l_btn_handle, BUTTON_PRESS_DOWN, NULL, on_volume_down, ctx);
    iot_button_register_cb(l_btn_handle, BUTTON_LONG_PRESS_START, NULL, on_volume_down_long, ctx);
    iot_button_register_cb(m_btn_handle, BUTTON_PRESS_DOWN, NULL, on_middle_click, ctx);
    iot_button_register_cb(r_btn_handle, BUTTON_PRESS_DOWN, NULL, on_volume_up, ctx);
    iot_button_register_cb(r_btn_handle, BUTTON_LONG_PRESS_START, NULL, on_volume_up_long, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_audio_codec(board_desc_t *self)
{
    atk_dnesp32s3_box2_4g_ctx_t *ctx = (atk_dnesp32s3_box2_4g_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8389_codec_create(ctx->i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, AUDIO_CODEC_ES8389_ADDR, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    atk_dnesp32s3_box2_4g_ctx_t *ctx = (atk_dnesp32s3_box2_4g_ctx_t *)self;
    return ctx->display;
}

static void *get_backlight(board_desc_t *self)
{
    atk_dnesp32s3_box2_4g_ctx_t *ctx = (atk_dnesp32s3_box2_4g_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void board_destroy(board_desc_t *self)
{
    atk_dnesp32s3_box2_4g_ctx_t *ctx = (atk_dnesp32s3_box2_4g_ctx_t *)self;
    if (ctx->backlight) {
        backlight_destroy(ctx->backlight);
        ctx->backlight = NULL;
    }
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    atk_dnesp32s3_box2_4g_ctx_t *ctx = calloc(1, sizeof(atk_dnesp32s3_box2_4g_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_DUAL;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = board_destroy;
    ctx->base.modem_tx_pin = Module_4G_TX_PIN;
    ctx->base.modem_rx_pin = Module_4G_RX_PIN;
    ctx->base.modem_dtr_pin = GPIO_NUM_NC;
    ctx->base.default_net_type = 1;

    init_i2c(ctx);
    init_io_expander(ctx);
    init_display(ctx);
    init_buttons(ctx);
    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    backlight_restore_brightness(ctx->backlight);

    return &ctx->base;
}
