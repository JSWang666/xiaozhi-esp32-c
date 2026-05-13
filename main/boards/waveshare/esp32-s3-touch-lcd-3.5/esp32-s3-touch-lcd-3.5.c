#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "backlight.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>
#include "esp_io_expander_tca9554.h"
#include <esp_ota_ops.h>

#define TAG "waveshare_lcd_3_5"

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} st7796_lcd_init_cmd_t;

typedef struct {
    const st7796_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
} st7796_vendor_config_t;

static st7796_lcd_init_cmd_t st7796_lcd_init_cmds[] = {
    {0x11, (uint8_t []){ 0x00 }, 0, 120},
    {0x3A, (uint8_t []){ 0x05 }, 1, 0},
    {0xF0, (uint8_t []){ 0xC3 }, 1, 0},
    {0xF0, (uint8_t []){ 0x96 }, 1, 0},
    {0xB4, (uint8_t []){ 0x01 }, 1, 0},
    {0xB7, (uint8_t []){ 0xC6 }, 1, 0},
    {0xC0, (uint8_t []){ 0x80, 0x45 }, 2, 0},
    {0xC1, (uint8_t []){ 0x13 }, 1, 0},
    {0xC2, (uint8_t []){ 0xA7 }, 1, 0},
    {0xC5, (uint8_t []){ 0x0A }, 1, 0},
    {0xE8, (uint8_t []){ 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33 }, 8, 0},
    {0xE0, (uint8_t []){ 0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31 }, 14, 0},
    {0xE1, (uint8_t []){ 0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32 }, 14, 0},
    {0xF0, (uint8_t []){ 0x3C }, 1, 0},
    {0xF0, (uint8_t []){ 0x69 }, 1, 120},
    {0x21, (uint8_t []){ 0x00 }, 0, 0},
    {0x29, (uint8_t []){ 0x00 }, 0, 0},
};

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_io_expander_handle_t io_expander;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
} ws35_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void init_i2c(ws35_ctx_t *ctx)
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

static void init_tca9554(ws35_ctx_t *ctx)
{
    esp_err_t ret = esp_io_expander_new_i2c_tca9554(ctx->i2c_bus,
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &ctx->io_expander);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "TCA9554 create returned error");

    ret = esp_io_expander_set_dir(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT);
    ESP_ERROR_CHECK(ret);
    vTaskDelay(pdMS_TO_TICKS(100));
    ret = esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 0);
    ESP_ERROR_CHECK(ret);
    vTaskDelay(pdMS_TO_TICKS(100));
    ret = esp_io_expander_set_level(ctx->io_expander, IO_EXPANDER_PIN_NUM_1, 1);
    ESP_ERROR_CHECK(ret);
}

static void init_spi(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_PIN,
        .miso_io_num = DISPLAY_MISO_PIN,
        .sclk_io_num = DISPLAY_CLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_lcd_display(ws35_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = DISPLAY_SPI_MODE,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &ctx->panel_io));

    st7796_vendor_config_t st7796_vendor_config = {
        .init_cmds = st7796_lcd_init_cmds,
        .init_cmds_size = sizeof(st7796_lcd_init_cmds) / sizeof(st7796_lcd_init_cmd_t),
    };

    ESP_LOGI(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RST_PIN,
        .rgb_ele_order = DISPLAY_RGB_ORDER,
        .bits_per_pixel = 16,
        .vendor_config = &st7796_vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel));

    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(ws35_ctx_t *ctx)
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

static void *get_audio_codec(board_desc_t *self)
{
    ws35_ctx_t *ctx = (ws35_ctx_t *)self;
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
    ws35_ctx_t *ctx = (ws35_ctx_t *)self;
    return ctx->display;
}

static void *get_backlight(board_desc_t *self)
{
    ws35_ctx_t *ctx = (ws35_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(
            DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void destroy(board_desc_t *self)
{
    ws35_ctx_t *ctx = (ws35_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    ws35_ctx_t *ctx = calloc(1, sizeof(ws35_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    init_i2c(ctx);
    init_tca9554(ctx);
    init_spi();
    init_lcd_display(ctx);

    if (esp_reset_reason() == ESP_RST_POWERON) {
        fflush(stdout);
        esp_restart();
    }

    init_buttons(ctx);
    backlight_restore_brightness(get_backlight(&ctx->base));

    return &ctx->base;
}
