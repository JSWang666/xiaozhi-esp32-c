#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/codec_c_api.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "led/single_led.h"
#include "assets/lang_c.h"
#include "device_state.h"
#include "backlight.h"
#include "i2c_device.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>

#define TAG "ZhengchenCamBoard_ML307"

void InitializeMCPController(void);

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    i2c_device_t *pca9557;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
    board_btn_t *volume_up_button;
    board_btn_t *volume_down_button;

    audio_codec_ops_t wrapped_ops;
    const audio_codec_ops_t *orig_ops;
} zcb_ml307_ctx_t;

static void pca9557_set_output_state(i2c_device_t *dev, uint8_t bit, uint8_t level)
{
    uint8_t data = i2c_device_read_reg(dev, 0x01);
    data = (data & ~(1 << bit)) | (level << bit);
    i2c_device_write_reg(dev, 0x01, data);
}

static void wrapped_enable_output(audio_codec_t *codec, bool enable)
{
    zcb_ml307_ctx_t *ctx = (zcb_ml307_ctx_t *)((char *)codec -
        offsetof(zcb_ml307_ctx_t, codec) + offsetof(zcb_ml307_ctx_t, base));
    (void)ctx;

    zcb_ml307_ctx_t *real_ctx = NULL;
    board_desc_t *b = (board_desc_t *)((char *)&codec - offsetof(zcb_ml307_ctx_t, codec));
    real_ctx = (zcb_ml307_ctx_t *)b;
    (void)real_ctx;

    /* The wrapped ops approach: use a global since the codec pointer doesn't embed ctx */
    /* Fallback: just call original and skip PCA control if we can't recover ctx */
}

static zcb_ml307_ctx_t *g_ml307_ctx;

static void ml307_wrapped_enable_output(audio_codec_t *codec, bool enable)
{
    if (g_ml307_ctx && g_ml307_ctx->orig_ops && g_ml307_ctx->orig_ops->enable_output)
        g_ml307_ctx->orig_ops->enable_output(codec, enable);

    if (g_ml307_ctx && g_ml307_ctx->pca9557)
        pca9557_set_output_state(g_ml307_ctx->pca9557, 1, enable ? 1 : 0);
}

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    app_toggle_chat(app);
}

static void on_volume_up_click(void *ud)
{
    zcb_ml307_ctx_t *ctx = (zcb_ml307_ctx_t *)ud;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume + 10;
    if (vol > 100) vol = 100;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol / 10);
    if (ctx->display)
        display_show_notification(ctx->display, buf, 0);
}

static void on_volume_up_long(void *ud)
{
    zcb_ml307_ctx_t *ctx = (zcb_ml307_ctx_t *)ud;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_max_volume, 0);
}

static void on_volume_down_click(void *ud)
{
    zcb_ml307_ctx_t *ctx = (zcb_ml307_ctx_t *)ud;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume - 10;
    if (vol < 0) vol = 0;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol / 10);
    if (ctx->display)
        display_show_notification(ctx->display, buf, 0);
}

static void on_volume_down_long(void *ud)
{
    zcb_ml307_ctx_t *ctx = (zcb_ml307_ctx_t *)ud;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_muted, 0);
}

static void init_i2c(zcb_ml307_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = (i2c_port_t)1,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->i2c_bus));

    ctx->pca9557 = i2c_device_create(ctx->i2c_bus, 0x19);
    i2c_device_write_reg(ctx->pca9557, 0x01, 0x03);
    i2c_device_write_reg(ctx->pca9557, 0x03, 0xf8);
}

static void init_spi(zcb_ml307_ctx_t *ctx)
{
    (void)ctx;
    spi_bus_config_t buscfg = {
        .mosi_io_num = GPIO_NUM_40,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = GPIO_NUM_41,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_st7789_display(zcb_ml307_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_NUM_NC,
        .dc_gpio_num = GPIO_NUM_39,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    pca9557_set_output_state(ctx->pca9557, 0, 0);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(zcb_ml307_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t vol_up_cfg = { .gpio_num = VOLUME_UP_BUTTON_GPIO };
    ctx->volume_up_button = board_btn_create_gpio(&vol_up_cfg);
    board_btn_on_click(ctx->volume_up_button, on_volume_up_click, ctx);
    board_btn_on_long_press(ctx->volume_up_button, on_volume_up_long, ctx);

    board_btn_gpio_cfg_t vol_down_cfg = { .gpio_num = VOLUME_DOWN_BUTTON_GPIO };
    ctx->volume_down_button = board_btn_create_gpio(&vol_down_cfg);
    board_btn_on_click(ctx->volume_down_button, on_volume_down_click, ctx);
    board_btn_on_long_press(ctx->volume_down_button, on_volume_down_long, ctx);
}

static const char *zcbm_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *zcbm_get_led(board_desc_t *self)
{
    zcb_ml307_ctx_t *ctx = (zcb_ml307_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *zcbm_get_audio_codec(board_desc_t *self)
{
    zcb_ml307_ctx_t *ctx = (zcb_ml307_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = box_codec_create(
            ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);

        if (ctx->codec && ctx->codec->ops) {
            ctx->orig_ops = ctx->codec->ops;
            ctx->wrapped_ops = *ctx->codec->ops;
            ctx->wrapped_ops.enable_output = ml307_wrapped_enable_output;
            ctx->codec->ops = &ctx->wrapped_ops;
        }
    }
    return ctx->codec;
}

static void *zcbm_get_display(board_desc_t *self)
{
    zcb_ml307_ctx_t *ctx = (zcb_ml307_ctx_t *)self;
    return ctx->display;
}

static void *zcbm_get_backlight(board_desc_t *self)
{
    zcb_ml307_ctx_t *ctx = (zcb_ml307_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void zcbm_destroy(board_desc_t *self)
{
    zcb_ml307_ctx_t *ctx = (zcb_ml307_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->volume_up_button);
    board_btn_delete(ctx->volume_down_button);
    if (ctx->pca9557) i2c_device_destroy(ctx->pca9557);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    zcb_ml307_ctx_t *ctx = calloc(1, sizeof(zcb_ml307_ctx_t));
    if (!ctx) return NULL;
    g_ml307_ctx = ctx;

    ctx->base.kind = BOARD_KIND_DUAL;
    ctx->base.get_board_type = zcbm_get_board_type;
    ctx->base.get_led = zcbm_get_led;
    ctx->base.get_audio_codec = zcbm_get_audio_codec;
    ctx->base.get_display = zcbm_get_display;
    ctx->base.get_backlight = zcbm_get_backlight;
    ctx->base.destroy = zcbm_destroy;

    init_i2c(ctx);
    init_spi(ctx);
    init_st7789_display(ctx);
    init_buttons(ctx);
    InitializeMCPController();
    backlight_restore_brightness(zcbm_get_backlight(&ctx->base));

    return &ctx->base;
}
