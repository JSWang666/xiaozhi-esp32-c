#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "audio/codecs/no_audio_codec.h"
#include "led/single_led.h"
#include "assets/lang_c.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include "c_api/board_c_api.h"

#define TAG "atk_dnesp32s3_box"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;

    board_btn_t *boot_button;
    bool es8311_detected;
} atk_dnesp32s3_box_ctx_t;

static void xl9555_write_reg(i2c_master_bus_handle_t bus, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) return;
    i2c_master_transmit(dev, buf, 2, -1);
    i2c_master_bus_rm_device(dev);
}

static uint8_t xl9555_read_reg(i2c_master_bus_handle_t bus, uint8_t addr, uint8_t reg)
{
    uint8_t val = 0;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) return 0;
    i2c_master_transmit_receive(dev, &reg, 1, &val, 1, -1);
    i2c_master_bus_rm_device(dev);
    return val;
}

static void xl9555_set_output(i2c_master_bus_handle_t bus, uint8_t addr, uint8_t bit, uint8_t level)
{
    uint8_t data;
    int index = bit;
    if (bit < 8) {
        data = xl9555_read_reg(bus, addr, 0x02);
    } else {
        data = xl9555_read_reg(bus, addr, 0x03);
        index -= 8;
    }
    data = (data & ~(1 << index)) | (level << index);
    if (bit < 8) {
        xl9555_write_reg(bus, addr, 0x02, data);
    } else {
        xl9555_write_reg(bus, addr, 0x03, data);
    }
}

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

static void init_i2c(atk_dnesp32s3_box_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_48,
        .scl_io_num = GPIO_NUM_45,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->i2c_bus));

    xl9555_write_reg(ctx->i2c_bus, 0x20, 0x06, 0x3B);
    xl9555_write_reg(ctx->i2c_bus, 0x20, 0x07, 0xFE);

    uint8_t input0 = xl9555_read_reg(ctx->i2c_bus, 0x20, 0x00);
    ctx->es8311_detected = (input0 & 0x20) ? true : false;

    xl9555_write_reg(ctx->i2c_bus, 0x20, 0x06, 0x1B);
    xl9555_write_reg(ctx->i2c_bus, 0x20, 0x07, 0xFE);
}

static void init_display(atk_dnesp32s3_box_ctx_t *ctx)
{
    gpio_config_t gpio_init_struct = {
        .pin_bit_mask = 1ull << LCD_NUM_RD,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_init_struct);
    gpio_set_level(LCD_NUM_RD, 1);

    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = LCD_NUM_DC,
        .wr_gpio_num = LCD_NUM_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {
            GPIO_LCD_D0, GPIO_LCD_D1, GPIO_LCD_D2, GPIO_LCD_D3,
            GPIO_LCD_D4, GPIO_LCD_D5, GPIO_LCD_D6, GPIO_LCD_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = LCD_NUM_CS,
        .pclk_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &ctx->panel_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel));

    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
    esp_lcd_panel_set_gap(ctx->panel, 0, 0);

    uint8_t data0[] = {0x00};
    uint8_t data1[] = {0x65};
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0x36, data0, 1);
    esp_lcd_panel_io_tx_param(ctx->panel_io, 0x3A, data1, 1);
    esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_disp_on_off(ctx->panel, true);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(atk_dnesp32s3_box_ctx_t *ctx)
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
    atk_dnesp32s3_box_ctx_t *ctx = (atk_dnesp32s3_box_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    atk_dnesp32s3_box_ctx_t *ctx = (atk_dnesp32s3_box_ctx_t *)self;
    if (!ctx->codec) {
        if (ctx->es8311_detected) {
            ctx->codec = es8311_codec_create(ctx->i2c_bus, I2C_NUM_0,
                AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                GPIO_NUM_NC, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                GPIO_NUM_NC, AUDIO_CODEC_ES8311_ADDR, false, false);
        } else {
            ctx->codec = no_audio_codec_duplex_create(
                AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        }
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    atk_dnesp32s3_box_ctx_t *ctx = (atk_dnesp32s3_box_ctx_t *)self;
    return ctx->display;
}

static void board_destroy(board_desc_t *self)
{
    atk_dnesp32s3_box_ctx_t *ctx = (atk_dnesp32s3_box_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    atk_dnesp32s3_box_ctx_t *ctx = calloc(1, sizeof(atk_dnesp32s3_box_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.destroy = board_destroy;

    init_i2c(ctx);
    init_display(ctx);
    xl9555_set_output(ctx->i2c_bus, 0x20, 5, 1);
    xl9555_set_output(ctx->i2c_bus, 0x20, 7, 1);
    init_buttons(ctx);

    return &ctx->base;
}
