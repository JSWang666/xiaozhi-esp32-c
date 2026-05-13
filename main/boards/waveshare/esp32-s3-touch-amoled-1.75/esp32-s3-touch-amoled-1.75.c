#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "c_api/codec_c_api.h"
#include "display/display.h"
#include "backlight.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_spi.h>

#include "esp_lcd_co5300.h"
#include "esp_io_expander_tca9554.h"

#define TAG "WaveshareAMOLED1inch75"

#define LCD_OPCODE_WRITE_CMD (0x02ULL)

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    axp2101_t *pmic;
    esp_io_expander_handle_t io_expander;

    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;

    power_save_timer_t *power_save_timer;
} board_ctx_t;

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

static void custom_backlight_set(void *impl_ctx, uint8_t brightness)
{
    board_ctx_t *ctx = (board_ctx_t *)impl_ctx;
    uint8_t data = (uint8_t)((255 * brightness) / 100);
    int lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
    esp_lcd_panel_io_tx_param(ctx->panel_io, lcd_cmd, &data, sizeof(data));
}

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void on_enter_sleep(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    if (ctx->display)
        display_set_power_save_mode(ctx->display, true);
    if (ctx->backlight)
        backlight_set_brightness(ctx->backlight, 20, false);
}

static void on_exit_sleep(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    if (ctx->display)
        display_set_power_save_mode(ctx->display, false);
    if (ctx->backlight)
        backlight_restore_brightness(ctx->backlight);
}

static void on_shutdown(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    if (ctx->pmic)
        axp2101_power_off(ctx->pmic);
}

static void init_pmic(board_ctx_t *ctx)
{
    ctx->pmic = axp2101_create(ctx->i2c_bus, 0x34);
    i2c_device_t *dev = axp2101_get_i2c_device(ctx->pmic);

    i2c_device_write_reg(dev, 0x22, 0x06);
    i2c_device_write_reg(dev, 0x27, 0x10);
    i2c_device_write_reg(dev, 0x80, 0x01);
    i2c_device_write_reg(dev, 0x90, 0x00);
    i2c_device_write_reg(dev, 0x91, 0x00);
    i2c_device_write_reg(dev, 0x82, (3300 - 1500) / 100);
    i2c_device_write_reg(dev, 0x92, (3300 - 500) / 100);
    i2c_device_write_reg(dev, 0x90, 0x01);
    i2c_device_write_reg(dev, 0x64, 0x02);
    i2c_device_write_reg(dev, 0x61, 0x02);
    i2c_device_write_reg(dev, 0x62, 0x08);
    i2c_device_write_reg(dev, 0x63, 0x01);
}

static void init_i2c(board_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->i2c_bus));
}

static void init_tca9554(board_ctx_t *ctx)
{
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75
    esp_err_t ret = esp_io_expander_new_i2c_tca9554(ctx->i2c_bus, I2C_ADDRESS, &ctx->io_expander);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "TCA9554 create returned error");
    ret = esp_io_expander_set_dir(ctx->io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
    ESP_ERROR_CHECK(ret);
#endif
}

static void init_spi(board_ctx_t *ctx)
{
    spi_bus_config_t buscfg = {
        .data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0,
        .data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1,
        .sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK,
        .data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2,
        .data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_display(board_ctx_t *ctx)
{
    esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(
        EXAMPLE_PIN_NUM_LCD_CS, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &ctx->panel_io));

    const co5300_vendor_config_t vendor_config = {
        .init_cmds = &vendor_specific_init[0],
        .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
        .flags = {
            .use_qspi_interface = 1,
        }
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(ctx->panel_io, &panel_config, &ctx->panel));
    esp_lcd_panel_set_gap(ctx->panel, 0x06, 0);
    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, false);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_disp_on_off(ctx->panel, true);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

    backlight_impl_t bl_impl = {
        .set_brightness = custom_backlight_set,
        .destroy = NULL,
        .impl_ctx = ctx,
    };
    ctx->backlight = backlight_create(&bl_impl);
    backlight_restore_brightness(ctx->backlight);
}

static void init_buttons(board_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static void init_power_save(board_ctx_t *ctx)
{
    power_save_timer_cfg_t cfg = {
        .cpu_max_freq = -1,
        .seconds_to_sleep = 60,
        .seconds_to_shutdown = 300,
    };
    ctx->power_save_timer = power_save_timer_create(&cfg);
    power_save_timer_on_enter_sleep(ctx->power_save_timer, on_enter_sleep, ctx);
    power_save_timer_on_exit_sleep(ctx->power_save_timer, on_exit_sleep, ctx);
    power_save_timer_on_shutdown(ctx->power_save_timer, on_shutdown, ctx);
    power_save_timer_set_enabled(ctx->power_save_timer, true);
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
        ctx->codec = box_codec_create(ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    return ctx->display;
}

static void *get_backlight(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    return ctx->backlight;
}

static bool get_battery_level(board_desc_t *self, int *level, bool *charging, bool *discharging)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    static bool last_discharging = false;

    *charging = axp2101_is_charging(ctx->pmic);
    *discharging = axp2101_is_discharging(ctx->pmic);
    if (*discharging != last_discharging) {
        power_save_timer_set_enabled(ctx->power_save_timer, *discharging);
        last_discharging = *discharging;
    }
    *level = axp2101_get_battery_level(ctx->pmic);
    return true;
}

static void board_destroy(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    if (ctx->power_save_timer) power_save_timer_destroy(ctx->power_save_timer);
    if (ctx->pmic) axp2101_destroy(ctx->pmic);
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
    ctx->base.get_backlight = get_backlight;
    ctx->base.get_battery_level = get_battery_level;
    ctx->base.destroy = board_destroy;

    init_power_save(ctx);
    init_i2c(ctx);
    init_tca9554(ctx);
    init_pmic(ctx);
    init_spi(ctx);
    init_display(ctx);
    init_buttons(ctx);

    return &ctx->base;
}
