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
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>
#include "esp_io_expander_tca95xx_16bit.h"

#define TAG "atk_dnesp32s3_box3"

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
} atk_dnesp32s3_box3_ctx_t;

static atk_dnesp32s3_box3_ctx_t *s_instance;

static uint8_t io_expander_get_level(atk_dnesp32s3_box3_ctx_t *ctx, uint16_t pin_mask)
{
    uint32_t pin_val = 0;
    esp_io_expander_get_level(ctx->io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
    pin_mask &= DRV_IO_EXP_INPUT_MASK;
    return (uint8_t)((pin_val & pin_mask) ? 1 : 0);
}

static void on_volume_down(void *button_handle, void *usr_data)
{
    atk_dnesp32s3_box3_ctx_t *ctx = (atk_dnesp32s3_box3_ctx_t *)usr_data;
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
    atk_dnesp32s3_box3_ctx_t *ctx = (atk_dnesp32s3_box3_ctx_t *)usr_data;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
    if (ctx->display) display_show_notification(ctx->display, lang_str_muted, 0);
}

static void on_k2_click(void *button_handle, void *usr_data)
{
    (void)usr_data;
    app_context_t *app = app_get_context();
    if (!app) return;
    app_toggle_chat(app);
}

static void on_boot_click(void *button_handle, void *usr_data)
{
    atk_dnesp32s3_box3_ctx_t *ctx = (atk_dnesp32s3_box3_ctx_t *)usr_data;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume + 10;
    if (vol > 100) vol = 100;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display) display_show_notification(ctx->display, buf, 0);
}

static void on_boot_long(void *button_handle, void *usr_data)
{
    atk_dnesp32s3_box3_ctx_t *ctx = (atk_dnesp32s3_box3_ctx_t *)usr_data;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
    if (ctx->display) display_show_notification(ctx->display, lang_str_max_volume, 0);
}

static uint8_t xio_k1_get_key_level(button_driver_t *drv)
{
    return !io_expander_get_level(s_instance, XIO_KEY_K1);
}

static uint8_t xio_k2_get_key_level(button_driver_t *drv)
{
    return io_expander_get_level(s_instance, XIO_KEY_K2);
}

static void init_i2c(atk_dnesp32s3_box3_ctx_t *ctx)
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

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_MOSI_PIN,
        .miso_io_num = LCD_MISO_PIN,
        .sclk_io_num = LCD_SCLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_io_expander(atk_dnesp32s3_box3_ctx_t *ctx)
{
    esp_io_expander_new_i2c_tca95xx_16bit(ctx->i2c_bus, AW9523B_ADDR, &ctx->io_exp_handle);

    esp_io_expander_set_dir(ctx->io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_dir(ctx->io_exp_handle, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);

    esp_io_expander_set_level(ctx->io_exp_handle, XIO_VDD_2V8_EN, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_VDD_3V3_EN, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_ESP_ADC_SEL, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_VDDA_3V3_EN, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_VBAT_EN, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_PA_CTRL, 1);
    esp_io_expander_set_level(ctx->io_exp_handle, XIO_LCD_BL, 0);
}

static void init_display(atk_dnesp32s3_box3_ctx_t *ctx)
{
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS_PIN,
        .dc_gpio_num = LCD_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = 60 * 1000 * 1000,
        .trans_queue_depth = 7,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &ctx->panel_io);

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    };
    esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel);
    esp_lcd_panel_reset(ctx->panel);

    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
    esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(atk_dnesp32s3_box3_ctx_t *ctx)
{
    s_instance = ctx;

    button_config_t k1_btn_cfg = { .long_press_time = 800, .short_press_time = 500 };
    button_config_t k2_btn_cfg = { .long_press_time = 800, .short_press_time = 500 };
    button_config_t bo_btn_cfg = { .long_press_time = 800, .short_press_time = 500 };

    button_handle_t k1_btn_handle = NULL;
    button_handle_t k2_btn_handle = NULL;
    button_handle_t bo_btn_handle = NULL;

    button_driver_t *xio_k1_btn_driver = (button_driver_t *)calloc(1, sizeof(button_driver_t));
    xio_k1_btn_driver->enable_power_save = false;
    xio_k1_btn_driver->get_key_level = xio_k1_get_key_level;
    ESP_ERROR_CHECK(iot_button_create(&k1_btn_cfg, xio_k1_btn_driver, &k1_btn_handle));

    button_driver_t *xio_k2_btn_driver = (button_driver_t *)calloc(1, sizeof(button_driver_t));
    xio_k2_btn_driver->enable_power_save = false;
    xio_k2_btn_driver->get_key_level = xio_k2_get_key_level;
    ESP_ERROR_CHECK(iot_button_create(&k2_btn_cfg, xio_k2_btn_driver, &k2_btn_handle));

    button_gpio_config_t bo_cfg = {
        .gpio_num = BOOT_BUTTON_GPIO,
        .active_level = BUTTON_INACTIVE,
        .enable_power_save = false,
        .disable_pull = false,
    };
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&bo_btn_cfg, &bo_cfg, &bo_btn_handle));

    iot_button_register_cb(k1_btn_handle, BUTTON_PRESS_DOWN, NULL, on_volume_down, ctx);
    iot_button_register_cb(k1_btn_handle, BUTTON_LONG_PRESS_START, NULL, on_volume_down_long, ctx);
    iot_button_register_cb(k2_btn_handle, BUTTON_PRESS_DOWN, NULL, on_k2_click, ctx);
    iot_button_register_cb(bo_btn_handle, BUTTON_PRESS_DOWN, NULL, on_boot_click, ctx);
    iot_button_register_cb(bo_btn_handle, BUTTON_LONG_PRESS_START, NULL, on_boot_long, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_audio_codec(board_desc_t *self)
{
    atk_dnesp32s3_box3_ctx_t *ctx = (atk_dnesp32s3_box3_ctx_t *)self;
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
    atk_dnesp32s3_box3_ctx_t *ctx = (atk_dnesp32s3_box3_ctx_t *)self;
    return ctx->display;
}

static void board_destroy(board_desc_t *self)
{
    atk_dnesp32s3_box3_ctx_t *ctx = (atk_dnesp32s3_box3_ctx_t *)self;
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    atk_dnesp32s3_box3_ctx_t *ctx = calloc(1, sizeof(atk_dnesp32s3_box3_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.destroy = board_destroy;

    init_i2c(ctx);
    init_spi();
    init_io_expander(ctx);
    init_display(ctx);
    init_buttons(ctx);

    return &ctx->base;
}
