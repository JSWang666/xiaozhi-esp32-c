#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "backlight.h"
#include "assets/lang_c.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_adc/adc_oneshot.h>
#include <button_adc.h>
#include "esp_lcd_ili9341.h"

#define TAG "EspBoxBoardLite"

extern audio_codec_t *box_codec_lite_create(void *i2c_master_handle,
    int input_sample_rate, int output_sample_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, bool input_reference);

typedef enum {
    BSP_ADC_BUTTON_PREV,
    BSP_ADC_BUTTON_ENTER,
    BSP_ADC_BUTTON_NEXT,
    BSP_ADC_BUTTON_NUM
} bsp_adc_button_t;

static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},
    {0xB1, (uint8_t []){00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},
    {0x11, (uint8_t []){0}, 0x80, 0},
    {0x29, (uint8_t []){0}, 0x80, 0},
    {0, (uint8_t []){0}, 0xff, 0},
};

typedef struct {
    board_desc_t base;
    i2c_master_bus_handle_t i2c_bus;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;
    board_btn_t *boot_button;
    board_btn_t *adc_button[BSP_ADC_BUTTON_NUM];
    adc_oneshot_unit_handle_t adc_handle;
} esp_box_lite_ctx_t;

static void on_toggle_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void on_volume_up_click(void *ud)
{
    esp_box_lite_ctx_t *ctx = (esp_box_lite_ctx_t *)ud;
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
    esp_box_lite_ctx_t *ctx = (esp_box_lite_ctx_t *)ud;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_max_volume, 0);
}

static void on_volume_down_click(void *ud)
{
    esp_box_lite_ctx_t *ctx = (esp_box_lite_ctx_t *)ud;
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
    esp_box_lite_ctx_t *ctx = (esp_box_lite_ctx_t *)ud;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_muted, 0);
}

static void init_i2c(esp_box_lite_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = 1,
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
        .mosi_io_num = GPIO_NUM_6,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = GPIO_NUM_7,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_display(esp_box_lite_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_NUM_5,
        .dc_gpio_num = GPIO_NUM_4,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    const ili9341_vendor_config_t vendor_config = {
        .init_cmds = &vendor_specific_init[0],
        .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_48,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    panel_config.flags.reset_active_high = 0;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_disp_on_off(panel, true);

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(esp_box_lite_ctx_t *ctx)
{
    button_adc_config_t adc_cfg = {0};
    adc_cfg.adc_channel = ADC_CHANNEL_0;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    const adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&init_config1, &ctx->adc_handle);
    adc_cfg.adc_handle = &ctx->adc_handle;
#endif

    adc_cfg.button_index = BSP_ADC_BUTTON_PREV;
    adc_cfg.min = 2310;
    adc_cfg.max = 2510;
    ctx->adc_button[0] = board_btn_create_adc(&adc_cfg);

    adc_cfg.button_index = BSP_ADC_BUTTON_ENTER;
    adc_cfg.min = 1880;
    adc_cfg.max = 2080;
    ctx->adc_button[1] = board_btn_create_adc(&adc_cfg);

    adc_cfg.button_index = BSP_ADC_BUTTON_NEXT;
    adc_cfg.min = 720;
    adc_cfg.max = 920;
    ctx->adc_button[2] = board_btn_create_adc(&adc_cfg);

    board_btn_on_click(ctx->adc_button[BSP_ADC_BUTTON_NEXT], on_volume_up_click, ctx);
    board_btn_on_long_press(ctx->adc_button[BSP_ADC_BUTTON_NEXT], on_volume_up_long, ctx);

    board_btn_on_click(ctx->adc_button[BSP_ADC_BUTTON_PREV], on_volume_down_click, ctx);
    board_btn_on_long_press(ctx->adc_button[BSP_ADC_BUTTON_PREV], on_volume_down_long, ctx);

    board_btn_on_click(ctx->adc_button[BSP_ADC_BUTTON_ENTER], on_toggle_click, ctx);

    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_toggle_click, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return "esp-box-lite";
}

static void *get_audio_codec(board_desc_t *self)
{
    esp_box_lite_ctx_t *ctx = (esp_box_lite_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = box_codec_lite_create(ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    return ((esp_box_lite_ctx_t *)self)->display;
}

static void *get_backlight(board_desc_t *self)
{
    esp_box_lite_ctx_t *ctx = (esp_box_lite_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void destroy(board_desc_t *self)
{
    esp_box_lite_ctx_t *ctx = (esp_box_lite_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    for (int i = 0; i < BSP_ADC_BUTTON_NUM; i++) {
        board_btn_delete(ctx->adc_button[i]);
    }
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    esp_box_lite_ctx_t *ctx = calloc(1, sizeof(esp_box_lite_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    init_i2c(ctx);
    init_spi();
    init_display(ctx);
    init_buttons(ctx);
    backlight_restore_brightness(get_backlight(&ctx->base));

    return &ctx->base;
}
