#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "backlight.h"
#include "power_save_timer.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_nv3023.h>
#include <esp_efuse_table.h>

#define TAG "magiclick_c3"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t codec_i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    power_save_timer_t *pst;

    board_btn_t *boot_button;
} magiclick_c3_ctx_t;

static void on_enter_sleep(void *ud)
{
    magiclick_c3_ctx_t *ctx = (magiclick_c3_ctx_t *)ud;
    display_set_power_save_mode(ctx->display, true);
    backlight_set_brightness(ctx->backlight, 10, false);
}

static void on_exit_sleep(void *ud)
{
    magiclick_c3_ctx_t *ctx = (magiclick_c3_ctx_t *)ud;
    display_set_power_save_mode(ctx->display, false);
    backlight_restore_brightness(ctx->backlight);
}

static void on_boot_click(void *ud)
{
    magiclick_c3_ctx_t *ctx = (magiclick_c3_ctx_t *)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) {
        return;
    }
    (void)ctx;
}

static void on_boot_press_down(void *ud)
{
    magiclick_c3_ctx_t *ctx = (magiclick_c3_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    app_context_t *app = app_get_context();
    if (app) app_start_listening(app);
}

static void on_boot_press_up(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_stop_listening(app);
}

static void init_codec_i2c(magiclick_c3_ctx_t *ctx)
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

    if (i2c_master_probe(ctx->codec_i2c_bus, 0x18, 1000) != ESP_OK) {
        while (true) {
            ESP_LOGE(TAG, "Failed to probe I2C bus, please check if you have installed the correct firmware");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }
}

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_SDA_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SCL_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_nv3023_display(magiclick_c3_ctx_t *ctx)
{
    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &ctx->panel_io));

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_nv3023(ctx->panel_io, &panel_config, &ctx->panel));

    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, false);
    esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_disp_on_off(ctx->panel, true);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(magiclick_c3_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
    board_btn_on_press_down(ctx->boot_button, on_boot_press_down, ctx);
    board_btn_on_press_up(ctx->boot_button, on_boot_press_up, ctx);
}

static void init_power_save(magiclick_c3_ctx_t *ctx)
{
    power_save_timer_cfg_t cfg = {
        .cpu_max_freq = 160,
        .seconds_to_sleep = 20,
        .seconds_to_shutdown = -1,
    };
    ctx->pst = power_save_timer_create(&cfg);
    power_save_timer_on_enter_sleep(ctx->pst, on_enter_sleep, ctx);
    power_save_timer_on_exit_sleep(ctx->pst, on_exit_sleep, ctx);
    power_save_timer_set_enabled(ctx->pst, true);
}

static const char *mc3_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *mc3_get_led(board_desc_t *self)
{
    magiclick_c3_ctx_t *ctx = (magiclick_c3_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *mc3_get_audio_codec(board_desc_t *self)
{
    magiclick_c3_ctx_t *ctx = (magiclick_c3_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->codec_i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, false);
    }
    return ctx->codec;
}

static void *mc3_get_display(board_desc_t *self)
{
    magiclick_c3_ctx_t *ctx = (magiclick_c3_ctx_t *)self;
    return ctx->display;
}

static void *mc3_get_backlight(board_desc_t *self)
{
    magiclick_c3_ctx_t *ctx = (magiclick_c3_ctx_t *)self;
    return ctx->backlight;
}

static void mc3_destroy(board_desc_t *self)
{
    magiclick_c3_ctx_t *ctx = (magiclick_c3_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    if (ctx->pst) power_save_timer_destroy(ctx->pst);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    magiclick_c3_ctx_t *ctx = calloc(1, sizeof(magiclick_c3_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = mc3_get_board_type;
    ctx->base.get_led = mc3_get_led;
    ctx->base.get_audio_codec = mc3_get_audio_codec;
    ctx->base.get_display = mc3_get_display;
    ctx->base.get_backlight = mc3_get_backlight;
    ctx->base.destroy = mc3_destroy;

    init_codec_i2c(ctx);
    init_spi();
    init_nv3023_display(ctx);

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
        DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);

    init_buttons(ctx);
    init_power_save(ctx);
    backlight_restore_brightness(ctx->backlight);

    esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);

    return &ctx->base;
}
