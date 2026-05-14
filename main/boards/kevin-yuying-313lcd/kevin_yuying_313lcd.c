#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "pin_config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "backlight.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include "esp_lcd_gc9503.h"
#include <esp_lcd_panel_io_additions.h>

#define TAG "Yuying_313lcd"

typedef struct {
    board_desc_t base;
    void *codec;
    void *display;
    void *backlight;
    i2c_master_bus_handle_t codec_i2c_bus;
} yuying_ctx_t;

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

static void init_codec_i2c(yuying_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->codec_i2c_bus));
}

static void *init_rgb_display(void)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;

    ESP_LOGI(TAG, "Install 3-wire SPI panel IO");
    spi_line_config_t line_config = {
        .cs_io_type = IO_TYPE_GPIO,
        .cs_gpio_num = GC9503V_LCD_IO_SPI_CS_1,
        .scl_io_type = IO_TYPE_GPIO,
        .scl_gpio_num = GC9503V_LCD_IO_SPI_SCL_1,
        .sda_io_type = IO_TYPE_GPIO,
        .sda_gpio_num = GC9503V_LCD_IO_SPI_SDO_1,
        .io_expander = NULL,
    };
    esp_lcd_panel_io_3wire_spi_config_t io_config = GC9503_PANEL_IO_3WIRE_SPI_CONFIG(line_config, 0);
    esp_lcd_new_panel_io_3wire_spi(&io_config, &panel_io);

    ESP_LOGI(TAG, "Install RGB LCD panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_rgb_panel_config_t rgb_config = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = GC9503_376_960_PANEL_60HZ_RGB_TIMING(),
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
            GC9503V_PIN_NUM_DATA0,  GC9503V_PIN_NUM_DATA1,
            GC9503V_PIN_NUM_DATA2,  GC9503V_PIN_NUM_DATA3,
            GC9503V_PIN_NUM_DATA4,  GC9503V_PIN_NUM_DATA5,
            GC9503V_PIN_NUM_DATA6,  GC9503V_PIN_NUM_DATA7,
            GC9503V_PIN_NUM_DATA8,  GC9503V_PIN_NUM_DATA9,
            GC9503V_PIN_NUM_DATA10, GC9503V_PIN_NUM_DATA11,
            GC9503V_PIN_NUM_DATA12, GC9503V_PIN_NUM_DATA13,
            GC9503V_PIN_NUM_DATA14, GC9503V_PIN_NUM_DATA15,
        },
        .flags = { .fb_in_psram = true },
    };

    gc9503_vendor_config_t vendor_config = {
        .rgb_config = &rgb_config,
        .flags = { .mirror_by_cmd = 0, .auto_del_panel_io = 1 },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    esp_lcd_new_panel_gc9503(panel_io, &panel_config, &panel_handle);
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);

    return rgb_lcd_display_create(panel_io, panel_handle,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static const char *get_board_type(board_desc_t *self) { (void)self; return "kevin-yuying-313lcd"; }
static void *get_led(board_desc_t *self) { (void)self; return NULL; }
static void *get_audio_codec(board_desc_t *self) { return ((yuying_ctx_t *)self)->codec; }
static void *get_display(board_desc_t *self) { return ((yuying_ctx_t *)self)->display; }
static void *get_backlight(board_desc_t *self) { return ((yuying_ctx_t *)self)->backlight; }

static void destroy(board_desc_t *self)
{
    yuying_ctx_t *ctx = (yuying_ctx_t *)self;
    if (ctx->backlight) {
        backlight_destroy(ctx->backlight);
        ctx->backlight = NULL;
    }
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    yuying_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    init_codec_i2c(ctx);

    ctx->codec = es8311_codec_create(ctx->codec_i2c_bus, I2C_NUM_0,
        AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
        AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
        AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, false);
    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 5000);

    ctx->display = init_rgb_display();

    board_btn_handle_t boot_btn = board_btn_create_gpio(BOOT_BUTTON_GPIO);
    board_btn_on_click(boot_btn, on_boot_click, NULL);
    board_btn_on_press_down(boot_btn, on_boot_press_down, NULL);
    board_btn_on_press_up(boot_btn, on_boot_press_up, NULL);

    return &ctx->base;
}
