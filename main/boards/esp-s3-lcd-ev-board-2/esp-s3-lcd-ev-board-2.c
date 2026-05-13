#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "pin_config.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include "esp_lcd_gc9503.h"
#include "device_state.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io_additions.h>
#include <esp_io_expander_tca9554.h>
#include <esp_lcd_touch_gt1151.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#define TAG "ESP_S3_LCD_EV_Board_2"

typedef struct {
    board_desc_t base;
    i2c_master_bus_handle_t i2c_bus;
    audio_codec_t *codec;
    display_t *display;
    led_t *led;
    board_btn_t *boot_button;
    esp_io_expander_handle_t expander;
} s3_lcd_ev2_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
}

static void on_boot_press_down(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_start_listening(app);
}

static void on_boot_press_up(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_stop_listening(app);
}

static void init_codec_i2c(s3_lcd_ev2_ctx_t *ctx)
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

    esp_io_expander_new_i2c_tca9554(ctx->i2c_bus, 0x20, &ctx->expander);
    esp_io_expander_set_dir(ctx->expander, BSP_POWER_AMP_IO, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(ctx->expander, BSP_POWER_AMP_IO, true);
}

static void init_buttons(s3_lcd_ev2_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
    board_btn_on_press_down(ctx->boot_button, on_boot_press_down, ctx);
    board_btn_on_press_up(ctx->boot_button, on_boot_press_up, ctx);
}

static void init_display(s3_lcd_ev2_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init GC9503V");

    esp_lcd_panel_io_handle_t panel_io = NULL;

    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(GC9503V_PIN_NUM_VSYNC),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(GC9503V_PIN_NUM_VSYNC, 1);

    spi_line_config_t line_config = {
        .cs_io_type = IO_TYPE_EXPANDER,
        .cs_expander_pin = GC9503V_LCD_IO_SPI_CS_1,
        .scl_io_type = IO_TYPE_EXPANDER,
        .scl_expander_pin = GC9503V_LCD_IO_SPI_SCL_1,
        .sda_io_type = IO_TYPE_EXPANDER,
        .sda_expander_pin = GC9503V_LCD_IO_SPI_SDO_1,
        .io_expander = ctx->expander,
    };

    esp_lcd_panel_io_3wire_spi_config_t io_config = GC9503_PANEL_IO_3WIRE_SPI_CONFIG(line_config, 0);
    esp_lcd_new_panel_io_3wire_spi(&io_config, &panel_io);

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_rgb_panel_config_t rgb_config = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = GC9503_800_480_PANEL_60HZ_RGB_TIMING(),
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = GC9503V_LCD_RGB_BUFFER_NUMS,
        .bounce_buffer_size_px = GC9503V_LCD_H_RES * GC9503V_LCD_RGB_BOUNCE_BUFFER_HEIGHT,
        .dma_burst_size = 64,
        .hsync_gpio_num = GC9503V_PIN_NUM_HSYNC,
        .vsync_gpio_num = GC9503V_PIN_NUM_VSYNC,
        .de_gpio_num = GC9503V_PIN_NUM_DE,
        .pclk_gpio_num = GC9503V_PIN_NUM_PCLK,
        .disp_gpio_num = GC9503V_PIN_NUM_DISP_EN,
        .data_gpio_nums = {
            GC9503V_PIN_NUM_DATA0, GC9503V_PIN_NUM_DATA1,
            GC9503V_PIN_NUM_DATA2, GC9503V_PIN_NUM_DATA3,
            GC9503V_PIN_NUM_DATA4, GC9503V_PIN_NUM_DATA5,
            GC9503V_PIN_NUM_DATA6, GC9503V_PIN_NUM_DATA7,
            GC9503V_PIN_NUM_DATA8, GC9503V_PIN_NUM_DATA9,
            GC9503V_PIN_NUM_DATA10, GC9503V_PIN_NUM_DATA11,
            GC9503V_PIN_NUM_DATA12, GC9503V_PIN_NUM_DATA13,
            GC9503V_PIN_NUM_DATA14, GC9503V_PIN_NUM_DATA15,
        },
        .flags = { .fb_in_psram = true },
    };

    gc9503_vendor_config_t vendor_config = {
        .rgb_config = &rgb_config,
        .flags = {
            .mirror_by_cmd = 0,
            .auto_del_panel_io = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 18,
        .vendor_config = &vendor_config,
    };
    esp_lcd_new_panel_gc9503(panel_io, &panel_config, &panel_handle);
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);

    ctx->display = rgb_lcd_display_create(panel_io, panel_handle,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_touch(s3_lcd_ev2_ctx_t *ctx)
{
    esp_lcd_touch_handle_t tp;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_WIDTH,
        .y_max = DISPLAY_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT1151_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = { .disable_control_phase = 1 },
    };
    tp_io_config.scl_speed_hz = 400 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(ctx->i2c_bus, &tp_io_config, &tp_io_handle));
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt1151(tp_io_handle, &tp_cfg, &tp));

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lv_display_get_default(),
        .handle = tp,
    };
    lvgl_port_add_touch(&touch_cfg);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return "esp-s3-lcd-ev-board-2";
}

static void *get_led(board_desc_t *self)
{
    s3_lcd_ev2_ctx_t *ctx = (s3_lcd_ev2_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    s3_lcd_ev2_ctx_t *ctx = (s3_lcd_ev2_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = box_codec_create(ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            true);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    return ((s3_lcd_ev2_ctx_t *)self)->display;
}

static void destroy(board_desc_t *self)
{
    s3_lcd_ev2_ctx_t *ctx = (s3_lcd_ev2_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    s3_lcd_ev2_ctx_t *ctx = calloc(1, sizeof(s3_lcd_ev2_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.destroy = destroy;

    init_codec_i2c(ctx);
    init_buttons(ctx);
    init_display(ctx);
    init_touch(ctx);

    return &ctx->base;
}
