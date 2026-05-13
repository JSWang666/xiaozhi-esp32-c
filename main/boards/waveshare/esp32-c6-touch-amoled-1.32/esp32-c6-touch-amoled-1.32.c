#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"

#include <stdlib.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include "esp_lcd_sh8601.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "waveshare_c6_amoled_1_32"

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]) {0x00}, 1, 0},
    {0xC4, (uint8_t[]) {0x80}, 1, 0},
    {0x3A, (uint8_t[]) {0x55}, 1, 0},
    {0x35, (uint8_t[]) {0x00}, 1, 0},
    {0x53, (uint8_t[]) {0x20}, 1, 0},
    {0x51, (uint8_t[]) {0xFF}, 1, 0},
    {0x63, (uint8_t[]) {0xFF}, 1, 0},
    {0x2A, (uint8_t[]) {0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]) {0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t[]) {0x00}, 0, 100},
    {0x29, (uint8_t[]) {0x00}, 0, 0},
};

typedef struct {
    board_desc_t base;
    i2c_master_bus_handle_t i2c_bus;
    esp_lcd_panel_io_handle_t io_handle;
    audio_codec_t *codec;
    display_t *display;
    board_btn_t *boot_button;
} board_ctx_t;

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
        ctx->codec = es8311_codec_create(ctx->i2c_bus, 0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    return ((board_ctx_t *)self)->display;
}

static void board_destroy(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

static void init_power(void)
{
    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1ULL << SYS_POWER_IO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio_conf));
    gpio_set_level(SYS_POWER_IO_PIN, 1);
    do {
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (!gpio_get_level(PWR_KEY_GPIO));
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

static void init_display(board_ctx_t *ctx)
{
    spi_bus_config_t buscfg = {0};
    buscfg.data0_io_num = LCD_D0;
    buscfg.data1_io_num = LCD_D1;
    buscfg.sclk_io_num = LCD_PCLK;
    buscfg.data2_io_num = LCD_D2;
    buscfg.data3_io_num = LCD_D3;
    buscfg.max_transfer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {0};
    io_config.cs_gpio_num = LCD_CS;
    io_config.dc_gpio_num = -1;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 8;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &ctx->io_handle));

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(ctx->io_handle, &panel_config, &panel));
    esp_lcd_panel_set_gap(panel, 0x06, 0x00);
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    ctx->display = spi_lcd_display_create(ctx->io_handle, panel,
        EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
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
    ctx->base.destroy = board_destroy;

    init_power();
    init_i2c(ctx);
    init_display(ctx);
    init_buttons(ctx);

    return &ctx->base;
}
