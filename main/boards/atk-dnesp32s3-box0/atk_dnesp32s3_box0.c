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

#define TAG "atk_dnesp32s3_box0"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *right_button;
    board_btn_t *left_button;
    board_btn_t *middle_button;
} atk_dnesp32s3_box0_ctx_t;

static void on_middle_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    app_toggle_chat(app);
}

static void on_volume_down_click(void *ud)
{
    atk_dnesp32s3_box0_ctx_t *ctx = (atk_dnesp32s3_box0_ctx_t *)ud;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume - 10;
    if (vol < 0) vol = 0;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display)
        display_show_notification(ctx->display, buf, 0);
}

static void on_volume_down_long(void *ud)
{
    atk_dnesp32s3_box0_ctx_t *ctx = (atk_dnesp32s3_box0_ctx_t *)ud;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_muted, 0);
}

static void on_volume_up_click(void *ud)
{
    atk_dnesp32s3_box0_ctx_t *ctx = (atk_dnesp32s3_box0_ctx_t *)ud;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume + 10;
    if (vol > 100) vol = 100;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display)
        display_show_notification(ctx->display, buf, 0);
}

static void on_volume_up_long(void *ud)
{
    atk_dnesp32s3_box0_ctx_t *ctx = (atk_dnesp32s3_box0_ctx_t *)ud;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_max_volume, 0);
}

static void init_power(void)
{
    gpio_config_t gpio_init_struct = {
        .pin_bit_mask = (1ull << CODEC_PWR_PIN) | (1ull << SYS_POW_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_init_struct);
    gpio_set_level(CODEC_PWR_PIN, 1);
    gpio_set_level(SYS_POW_PIN, 1);
}

static void init_i2c(atk_dnesp32s3_box0_ctx_t *ctx)
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
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = LCD_SCLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_display(atk_dnesp32s3_box0_ctx_t *ctx)
{
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS_PIN,
        .dc_gpio_num = LCD_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 7,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &ctx->panel_io);

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    };
    esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel);

    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, true);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(atk_dnesp32s3_box0_ctx_t *ctx)
{
    board_btn_gpio_cfg_t mid_cfg = { .gpio_num = M_BUTTON_GPIO };
    ctx->middle_button = board_btn_create_gpio(&mid_cfg);
    board_btn_on_click(ctx->middle_button, on_middle_click, ctx);

    board_btn_gpio_cfg_t left_cfg = { .gpio_num = L_BUTTON_GPIO };
    ctx->left_button = board_btn_create_gpio(&left_cfg);
    board_btn_on_click(ctx->left_button, on_volume_down_click, ctx);
    board_btn_on_long_press(ctx->left_button, on_volume_down_long, ctx);

    board_btn_gpio_cfg_t right_cfg = { .gpio_num = R_BUTTON_GPIO };
    ctx->right_button = board_btn_create_gpio(&right_cfg);
    board_btn_on_click(ctx->right_button, on_volume_up_click, ctx);
    board_btn_on_long_press(ctx->right_button, on_volume_up_long, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_led(board_desc_t *self)
{
    (void)self;
    return NULL;
}

static void *get_audio_codec(board_desc_t *self)
{
    atk_dnesp32s3_box0_ctx_t *ctx = (atk_dnesp32s3_box0_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            GPIO_NUM_NC, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, AUDIO_CODEC_ES8311_ADDR, false, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    atk_dnesp32s3_box0_ctx_t *ctx = (atk_dnesp32s3_box0_ctx_t *)self;
    return ctx->display;
}

static void *get_backlight(board_desc_t *self)
{
    atk_dnesp32s3_box0_ctx_t *ctx = (atk_dnesp32s3_box0_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void board_destroy(board_desc_t *self)
{
    atk_dnesp32s3_box0_ctx_t *ctx = (atk_dnesp32s3_box0_ctx_t *)self;
    if (ctx->backlight) {
        backlight_destroy(ctx->backlight);
        ctx->backlight = NULL;
    }
    board_btn_delete(ctx->middle_button);
    board_btn_delete(ctx->left_button);
    board_btn_delete(ctx->right_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    atk_dnesp32s3_box0_ctx_t *ctx = calloc(1, sizeof(atk_dnesp32s3_box0_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = board_destroy;

    init_power();
    init_i2c(ctx);
    init_spi();
    init_display(ctx);
    init_buttons(ctx);
    backlight_restore_brightness(ctx->backlight ? ctx->backlight :
        (ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000)));

    return &ctx->base;
}
