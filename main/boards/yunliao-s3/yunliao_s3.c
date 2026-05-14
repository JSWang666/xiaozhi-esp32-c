#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/codec_c_api.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "assets/lang_c.h"
#include "device_state.h"
#include "backlight.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "YunliaoS3"

#include "power_manager_c.h"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t codec_i2c_bus;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
    yunliao_pm_t *power_manager;
} yunliao_s3_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_toggle_chat(app);
}

static void init_i2c(yunliao_s3_ctx_t *ctx)
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
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->codec_i2c_bus));
}

static void init_spi(yunliao_s3_ctx_t *ctx)
{
    (void)ctx;
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_SPI_PIN_MOSI,
        .miso_io_num = DISPLAY_SPI_PIN_MISO,
        .sclk_io_num = DISPLAY_SPI_PIN_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_st7789_display(yunliao_s3_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_SPI_PIN_LCD_CS,
        .dc_gpio_num = DISPLAY_SPI_PIN_LCD_DC,
        .spi_mode = 3,
        .pclk_hz = DISPLAY_SPI_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_LCD_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_SPI_PIN_LCD_RST,
        .rgb_ele_order = DISPLAY_RGB_ORDER_COLOR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(yunliao_s3_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_PIN };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *yl_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *yl_get_audio_codec(board_desc_t *self)
{
    yunliao_s3_ctx_t *ctx = (yunliao_s3_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8388_codec_create(
            ctx->codec_i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8388_ADDR,
            AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *yl_get_display(board_desc_t *self)
{
    yunliao_s3_ctx_t *ctx = (yunliao_s3_ctx_t *)self;
    return ctx->display;
}

static void *yl_get_backlight(board_desc_t *self)
{
    yunliao_s3_ctx_t *ctx = (yunliao_s3_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static bool yl_get_battery_level(board_desc_t *self, int *level,
                                  bool *charging, bool *discharging)
{
    yunliao_s3_ctx_t *ctx = (yunliao_s3_ctx_t *)self;
    *level = yunliao_pm_get_battery_level(ctx->power_manager);
    *charging = yunliao_pm_is_charging(ctx->power_manager);
    *discharging = yunliao_pm_is_discharging(ctx->power_manager);
    return true;
}

static void yl_destroy(board_desc_t *self)
{
    yunliao_s3_ctx_t *ctx = (yunliao_s3_ctx_t *)self;
    if (ctx->backlight) {
        backlight_destroy(ctx->backlight);
        ctx->backlight = NULL;
    }
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    yunliao_s3_ctx_t *ctx = calloc(1, sizeof(yunliao_s3_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_DUAL;
    ctx->base.get_board_type = yl_get_board_type;
    ctx->base.get_audio_codec = yl_get_audio_codec;
    ctx->base.get_display = yl_get_display;
    ctx->base.get_backlight = yl_get_backlight;
    ctx->base.get_battery_level = yl_get_battery_level;
    ctx->base.destroy = yl_destroy;
    ctx->base.modem_tx_pin = ML307_TX_PIN;
    ctx->base.modem_rx_pin = ML307_RX_PIN;
    ctx->base.modem_dtr_pin = GPIO_NUM_NC;
    ctx->base.default_net_type = 1;

    ctx->power_manager = yunliao_pm_create();
    yunliao_pm_start_5v(ctx->power_manager);
    yunliao_pm_initialize(ctx->power_manager);

    init_i2c(ctx);
    yunliao_pm_check_startup(ctx->power_manager);
    init_spi(ctx);
    init_st7789_display(ctx);

    backlight_restore_brightness(yl_get_backlight(&ctx->base));

    while (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    init_buttons(ctx);

    yunliao_pm_start_4g(ctx->power_manager);
    yunliao_pm_init_bt_modul(ctx->power_manager);

    return &ctx->base;
}
