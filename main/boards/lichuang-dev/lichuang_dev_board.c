#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "backlight.h"
#include "i2c_device.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>
#include <esp_lcd_io_i2c.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#define TAG "LichuangDevBoard"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    i2c_device_t *pca9557;

    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
} lichuang_ctx_t;

static void pca9557_set_output(i2c_device_t *dev, uint8_t bit, uint8_t level)
{
    uint8_t data = i2c_device_read_reg(dev, 0x01);
    data = (data & ~(1 << bit)) | (level << bit);
    i2c_device_write_reg(dev, 0x01, data);
}

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void init_i2c(lichuang_ctx_t *ctx)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 1,
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
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &ctx->i2c_bus));

    ctx->pca9557 = i2c_device_create(ctx->i2c_bus, 0x19);
    i2c_device_write_reg(ctx->pca9557, 0x01, 0x03);
    i2c_device_write_reg(ctx->pca9557, 0x03, 0xf8);
}

static void init_spi(lichuang_ctx_t *ctx)
{
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

static void init_st7789_display(lichuang_ctx_t *ctx)
{
    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_NUM_NC,
        .dc_gpio_num = GPIO_NUM_39,
        .spi_mode = 2,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &ctx->panel_io));

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel));

    esp_lcd_panel_reset(ctx->panel);
    pca9557_set_output(ctx->pca9557, 0, 0);

    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, true);
    esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_disp_on_off(ctx->panel, true);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_touch(lichuang_ctx_t *ctx)
{
    esp_lcd_touch_handle_t tp;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_HEIGHT,
        .y_max = DISPLAY_WIDTH,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .flags = {
            .disable_control_phase = 1,
        },
        .scl_speed_hz = 400000,
    };

    esp_lcd_new_panel_io_i2c(ctx->i2c_bus, &tp_io_config, &tp_io_handle);
    esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp);

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lv_display_get_default(),
        .handle = tp,
    };
    if (touch_cfg.disp) {
        lvgl_port_add_touch(&touch_cfg);
    } else {
        ESP_LOGE(TAG, "Touch display is not initialized");
    }
}

static void init_buttons(lichuang_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *lcdev_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *lcdev_get_led(board_desc_t *self)
{
    (void)self;
    return NULL;
}

static void *lcdev_get_audio_codec(board_desc_t *self)
{
    lichuang_ctx_t *ctx = (lichuang_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = box_codec_create(ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        pca9557_set_output(ctx->pca9557, 1, 1);
    }
    return ctx->codec;
}

static void *lcdev_get_display(board_desc_t *self)
{
    lichuang_ctx_t *ctx = (lichuang_ctx_t *)self;
    return ctx->display;
}

static void *lcdev_get_backlight(board_desc_t *self)
{
    lichuang_ctx_t *ctx = (lichuang_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void lcdev_destroy(board_desc_t *self)
{
    lichuang_ctx_t *ctx = (lichuang_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    if (ctx->pca9557) i2c_device_destroy(ctx->pca9557);
    if (ctx->backlight) backlight_destroy(ctx->backlight);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    lichuang_ctx_t *ctx = calloc(1, sizeof(lichuang_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = lcdev_get_board_type;
    ctx->base.get_led = lcdev_get_led;
    ctx->base.get_audio_codec = lcdev_get_audio_codec;
    ctx->base.get_display = lcdev_get_display;
    ctx->base.get_backlight = lcdev_get_backlight;
    ctx->base.destroy = lcdev_destroy;

    init_i2c(ctx);
    init_spi(ctx);
    init_st7789_display(ctx);
    init_touch(ctx);
    init_buttons(ctx);

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
        DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    backlight_restore_brightness(ctx->backlight);

    return &ctx->base;
}
