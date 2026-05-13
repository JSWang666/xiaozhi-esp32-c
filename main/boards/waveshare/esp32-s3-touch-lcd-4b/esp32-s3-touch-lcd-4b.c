#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "backlight.h"

#include <stdlib.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_io_additions.h>
#include "esp_lcd_st7701.h"
#include "esp_io_expander_tca9554.h"
#include <esp_lcd_touch_gt911.h>
#include <esp_lcd_io_i2c.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "WaveshareEsp32s3TouchLCD4b"

static const st7701_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x21, 0x08}, 2, 0},
    {0xCD, (uint8_t[]){0x08}, 1, 0},
    {0xB0, (uint8_t[]){0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x60}, 1, 0},
    {0xB1, (uint8_t[]){0x30}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x49}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 20},
    {0xE0, (uint8_t[]){0x00, 0x1B, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x3C, 0x00}, 2, 0},
    {0xED, (uint8_t[]){0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x66}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

typedef struct {
    board_desc_t base;
    i2c_master_bus_handle_t i2c_bus;
    esp_io_expander_handle_t io_expander;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;
    board_btn_t *boot_button;
} board_ctx_t;

static void i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    i2c_master_transmit(dev, data, 2, -1);
}

static void init_axp2101(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));

    i2c_write_reg(dev, 0x22, 0x06);
    i2c_write_reg(dev, 0x27, 0x10);
    i2c_write_reg(dev, 0x80, 0x01);
    i2c_write_reg(dev, 0x90, 0x00);
    i2c_write_reg(dev, 0x91, 0x00);
    i2c_write_reg(dev, 0x82, (3300 - 1500) / 100);
    i2c_write_reg(dev, 0x92, (3300 - 500) / 100);
    i2c_write_reg(dev, 0x90, 0x01);
    i2c_write_reg(dev, 0x64, 0x02);
    i2c_write_reg(dev, 0x61, 0x02);
    i2c_write_reg(dev, 0x62, 0x08);
    i2c_write_reg(dev, 0x63, 0x01);
}

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
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
    board_ctx_t *ctx = (board_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = box_codec_create(ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    return ((board_ctx_t *)self)->display;
}

static void *get_backlight(board_desc_t *self)
{
    return ((board_ctx_t *)self)->backlight;
}

static void board_destroy(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

static void init_i2c(board_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->i2c_bus));
}

static void init_tca9554(board_ctx_t *ctx)
{
    esp_io_expander_new_i2c_tca9554(ctx->i2c_bus, I2C_ADDRESS, &ctx->io_expander);
    esp_io_expander_set_dir(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_3 | IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_6,
        IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(ctx->io_expander, IO_EXPANDER_PIN_NUM_3, 1);
    esp_io_expander_set_level(ctx->io_expander, IO_EXPANDER_PIN_NUM_6, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_io_expander_set_level(ctx->io_expander, IO_EXPANDER_PIN_NUM_5, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_io_expander_set_level(ctx->io_expander, IO_EXPANDER_PIN_NUM_5, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_io_expander_set_dir(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_4 | IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_INPUT);
}

static void init_display(board_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;

    spi_line_config_t line_config = {
        .cs_io_type = IO_TYPE_EXPANDER,
        .cs_expander_pin = BSP_LCD_IO_SPI_CS,
        .scl_io_type = IO_TYPE_EXPANDER,
        .scl_expander_pin = BSP_LCD_IO_SPI_SCL,
        .sda_io_type = IO_TYPE_EXPANDER,
        .sda_expander_pin = BSP_LCD_IO_SPI_SDA,
        .io_expander = ctx->io_expander,
    };
    esp_lcd_panel_io_3wire_spi_config_t io_config =
        ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_config, 0);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_3wire_spi(&io_config, &panel_io));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_rgb_panel_config_t rgb_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = DISPLAY_WIDTH,
            .v_res = DISPLAY_HEIGHT,
            .hsync_pulse_width = 10,
            .hsync_back_porch = 10,
            .hsync_front_porch = 20,
            .vsync_pulse_width = 10,
            .vsync_back_porch = 10,
            .vsync_front_porch = 10,
            .flags = { .pclk_active_neg = false },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        .bounce_buffer_size_px = 480 * 20,
        .psram_trans_align = 64,
        .hsync_gpio_num = BSP_LCD_HSYNC,
        .vsync_gpio_num = BSP_LCD_VSYNC,
        .de_gpio_num = BSP_LCD_DE,
        .pclk_gpio_num = BSP_LCD_PCLK,
        .disp_gpio_num = BSP_LCD_DISP,
        .data_gpio_nums = {
            BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
            BSP_LCD_DATA4, BSP_LCD_DATA5, BSP_LCD_DATA6, BSP_LCD_DATA7,
            BSP_LCD_DATA8, BSP_LCD_DATA9, BSP_LCD_DATA10, BSP_LCD_DATA11,
            BSP_LCD_DATA12, BSP_LCD_DATA13, BSP_LCD_DATA14, BSP_LCD_DATA15,
        },
        .flags = { .fb_in_psram = 1 },
    };

    st7701_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .rgb_config = &rgb_config,
        .flags = { .mirror_by_cmd = 0, .auto_del_panel_io = 1 },
    };

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 18,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(panel_io, &panel_config, &panel_handle));
    esp_lcd_panel_init(panel_handle);

    ctx->display = rgb_lcd_display_create(panel_io, panel_handle,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_touch(board_ctx_t *ctx)
{
    esp_lcd_touch_handle_t tp;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_WIDTH - 1,
        .y_max = DISPLAY_HEIGHT - 1,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 16,
        .flags = { .disable_control_phase = 1 },
    };
    tp_io_config.scl_speed_hz = 400 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(ctx->i2c_bus, &tp_io_config, &tp_io_handle));
    ESP_LOGI(TAG, "Initialize touch controller");
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp));
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lv_display_get_default(),
        .handle = tp,
    };
    lvgl_port_add_touch(&touch_cfg);
}

static void init_buttons(board_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

board_desc_t *create_board_desc(void)
{
    board_ctx_t *ctx = calloc(1, sizeof(board_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = board_destroy;

    init_i2c(ctx);
    init_tca9554(ctx);
    init_axp2101(ctx->i2c_bus);
    init_display(ctx);
    init_touch(ctx);
    init_buttons(ctx);
    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
        DISPLAY_BACKLIGHT_OUTPUT_INVERT, 5000);

    return &ctx->base;
}
