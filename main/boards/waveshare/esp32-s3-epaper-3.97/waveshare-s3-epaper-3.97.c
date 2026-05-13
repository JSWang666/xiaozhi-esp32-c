#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "axp2101.h"
#include "i2c_device.h"
#include "power_save_timer.h"
#include "device_state.h"

typedef struct {
    uint8_t cs;
    uint8_t dc;
    uint8_t rst;
    uint8_t busy;
    uint8_t mosi;
    uint8_t scl;
    int spi_host;
    int buffer_len;
} custom_epd_spi_t;

display_t *custom_epd_397_display_create(const custom_epd_spi_t *spi_cfg, int width, int height);

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "c_api/board_c_api.h"

#define TAG "waveshare_epaper_3_97"

#define BOARD_TYPE "WaveshareEsp32s3ePaper3inch97"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    axp2101_t *pmic;
    audio_codec_t *codec;
    display_t *display;

    board_btn_t *boot_button;
    board_btn_t *pwr_button;

    power_save_timer_t *power_save_timer;
} epaper397_ctx_t;

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

static void on_pwr_click(void *ud)
{
    epaper397_ctx_t *ctx = (epaper397_ctx_t *)ud;
    if (ctx->display)
        display_set_chat_message(ctx->display, "system", "OFF");
    vTaskDelay(pdMS_TO_TICKS(1000));
    axp2101_power_off(ctx->pmic);
}

static void on_shutdown(void *ud)
{
    epaper397_ctx_t *ctx = (epaper397_ctx_t *)ud;
    if (ctx->display)
        display_set_chat_message(ctx->display, "system", "OFF");
    vTaskDelay(pdMS_TO_TICKS(1000));
    axp2101_power_off(ctx->pmic);
}

static void init_i2c(epaper397_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = 0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->i2c_bus));
}

static void init_pmic(epaper397_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init AXP2101");
    ctx->pmic = axp2101_create(ctx->i2c_bus, 0x34);
    i2c_device_t *dev = axp2101_get_i2c_device(ctx->pmic);

    i2c_device_write_reg(dev, 0x22, 0x06);
    i2c_device_write_reg(dev, 0x27, 0x10);

    i2c_device_write_reg(dev, 0x80, 0x01);
    i2c_device_write_reg(dev, 0x90, 0x00);
    i2c_device_write_reg(dev, 0x91, 0x00);

    i2c_device_write_reg(dev, 0x82, (3300 - 1500) / 100);

    i2c_device_write_reg(dev, 0x92, (3300 - 500) / 100);
    i2c_device_write_reg(dev, 0x93, (3300 - 500) / 100);
    i2c_device_write_reg(dev, 0x94, (3300 - 500) / 100);

    i2c_device_write_reg(dev, 0x90, 0x07);

    i2c_device_write_reg(dev, 0x64, 0x03);
    i2c_device_write_reg(dev, 0x61, 0x02);
    i2c_device_write_reg(dev, 0x62, 0x08);
    i2c_device_write_reg(dev, 0x63, 0x01);
}

static void init_buttons(epaper397_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t pwr_cfg = { .gpio_num = VBAT_PWR_GPIO, .active_level = 1 };
    ctx->pwr_button = board_btn_create_gpio(&pwr_cfg);
    board_btn_on_click(ctx->pwr_button, on_pwr_click, ctx);
}

static void init_display(epaper397_ctx_t *ctx)
{
    custom_epd_spi_t spi_cfg = {
        .cs = EPD_CS_PIN,
        .dc = EPD_DC_PIN,
        .rst = EPD_RST_PIN,
        .busy = EPD_BUSY_PIN,
        .mosi = EPD_MOSI_PIN,
        .scl = EPD_SCK_PIN,
        .spi_host = EPD_SPI_NUM,
        .buffer_len = 48000,
    };
    ctx->display = custom_epd_397_display_create(&spi_cfg, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT);
}

static void init_power_save(epaper397_ctx_t *ctx)
{
    power_save_timer_cfg_t cfg = {
        .cpu_max_freq = -1,
        .seconds_to_sleep = 100,
        .seconds_to_shutdown = 300,
    };
    ctx->power_save_timer = power_save_timer_create(&cfg);
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
    epaper397_ctx_t *ctx = (epaper397_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, false, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    epaper397_ctx_t *ctx = (epaper397_ctx_t *)self;
    return ctx->display;
}

static bool get_battery_level(board_desc_t *self, int *level, bool *charging, bool *discharging)
{
    epaper397_ctx_t *ctx = (epaper397_ctx_t *)self;
    *charging = axp2101_is_charging(ctx->pmic);
    *discharging = axp2101_is_discharging(ctx->pmic);
    *level = axp2101_get_battery_level(ctx->pmic);
    return true;
}

static void board_destroy(board_desc_t *self)
{
    epaper397_ctx_t *ctx = (epaper397_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->pwr_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    epaper397_ctx_t *ctx = calloc(1, sizeof(epaper397_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_battery_level = get_battery_level;
    ctx->base.destroy = board_destroy;

    init_power_save(ctx);
    init_i2c(ctx);
    init_pmic(ctx);
    init_buttons(ctx);
    init_display(ctx);

    return &ctx->base;
}
