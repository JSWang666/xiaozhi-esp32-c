#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "c_api/codec_c_api.h"
#include "display/display.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_spi.h>

#include "esp_lcd_sh8601.h"

#define TAG "WaveshareAMOLED1inch32"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;

    board_btn_t *boot_button;
    board_btn_t *pwr_button;
} board_ctx_t;

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void on_pwr_long_press(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    if (ctx->display)
        display_set_chat_message(ctx->display, "system", "OFF");
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(PWR_EN_GPIO, 0);
}

static void check_power_key_state(void)
{
    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1ULL << PWR_EN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    gpio_set_level(PWR_EN_GPIO, 1);
    do {
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (!gpio_get_level(PWR_BUTTON_GPIO));
}

static void init_i2c(board_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
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
}

static void init_spi(board_ctx_t *ctx)
{
    spi_bus_config_t buscfg = {
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .sclk_io_num = LCD_PCLK,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .max_transfer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_display(board_ctx_t *ctx)
{
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 8,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = { .quad_mode = true },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &ctx->panel_io));

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

    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(ctx->panel_io, &panel_config, &ctx->panel));
    esp_lcd_panel_set_gap(ctx->panel, 0x06, 0x00);
    ESP_ERROR_CHECK(esp_lcd_panel_reset(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(ctx->panel));

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(board_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t pwr_cfg = { .gpio_num = PWR_BUTTON_GPIO };
    ctx->pwr_button = board_btn_create_gpio(&pwr_cfg);
    board_btn_on_long_press(ctx->pwr_button, on_pwr_long_press, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_audio_codec(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    return ctx->display;
}

static void board_destroy(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->pwr_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    board_ctx_t *ctx = calloc(1, sizeof(board_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.destroy = board_destroy;

    check_power_key_state();
    init_i2c(ctx);
    init_spi(ctx);
    init_display(ctx);
    init_buttons(ctx);

    return &ctx->base;
}
