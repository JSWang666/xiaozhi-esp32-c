#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "audio/codecs/no_audio_codec.h"
#include "backlight.h"
#include "assets/lang_c.h"
#include "adc_battery_monitor.h"
#include "power_save_timer.h"
#include "device_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>
#include <esp_lcd_nv3023.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include "c_api/board_c_api.h"

#define TAG "XINGZHI_CUBE_0_85TFT_WIFI"

static const nv3023_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xff, (uint8_t[]){0xa5}, 1, 0},
    {0x3E, (uint8_t[]){0x09}, 1, 0},
    {0x3A, (uint8_t[]){0x65}, 1, 0},
    {0x82, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x0f}, 1, 0},
    {0x64, (uint8_t[]){0x0f}, 1, 0},
    {0xB4, (uint8_t[]){0x34}, 1, 0},
    {0xB5, (uint8_t[]){0x30}, 1, 0},
    {0x83, (uint8_t[]){0x03}, 1, 0},
    {0x86, (uint8_t[]){0x04}, 1, 0},
    {0x87, (uint8_t[]){0x16}, 1, 0},
    {0x88, (uint8_t[]){0x0A}, 1, 0},
    {0x89, (uint8_t[]){0x27}, 1, 0},
    {0x93, (uint8_t[]){0x63}, 1, 0},
    {0x96, (uint8_t[]){0x81}, 1, 0},
    {0xC3, (uint8_t[]){0x10}, 1, 0},
    {0xE6, (uint8_t[]){0x00}, 1, 0},
    {0x99, (uint8_t[]){0x01}, 1, 0},
    {0x70, (uint8_t[]){0x09}, 1, 0},
    {0x71, (uint8_t[]){0x1D}, 1, 0},
    {0x72, (uint8_t[]){0x14}, 1, 0},
    {0x73, (uint8_t[]){0x0a}, 1, 0},
    {0x74, (uint8_t[]){0x11}, 1, 0},
    {0x75, (uint8_t[]){0x16}, 1, 0},
    {0x76, (uint8_t[]){0x38}, 1, 0},
    {0x77, (uint8_t[]){0x0B}, 1, 0},
    {0x78, (uint8_t[]){0x08}, 1, 0},
    {0x79, (uint8_t[]){0x3E}, 1, 0},
    {0x7a, (uint8_t[]){0x07}, 1, 0},
    {0x7b, (uint8_t[]){0x0D}, 1, 0},
    {0x7c, (uint8_t[]){0x16}, 1, 0},
    {0x7d, (uint8_t[]){0x0F}, 1, 0},
    {0x7e, (uint8_t[]){0x14}, 1, 0},
    {0x7f, (uint8_t[]){0x05}, 1, 0},
    {0xa0, (uint8_t[]){0x04}, 1, 0},
    {0xa1, (uint8_t[]){0x28}, 1, 0},
    {0xa2, (uint8_t[]){0x0c}, 1, 0},
    {0xa3, (uint8_t[]){0x11}, 1, 0},
    {0xa4, (uint8_t[]){0x0b}, 1, 0},
    {0xa5, (uint8_t[]){0x23}, 1, 0},
    {0xa6, (uint8_t[]){0x45}, 1, 0},
    {0xa7, (uint8_t[]){0x07}, 1, 0},
    {0xa8, (uint8_t[]){0x0a}, 1, 0},
    {0xa9, (uint8_t[]){0x3b}, 1, 0},
    {0xaa, (uint8_t[]){0x0d}, 1, 0},
    {0xab, (uint8_t[]){0x18}, 1, 0},
    {0xac, (uint8_t[]){0x14}, 1, 0},
    {0xad, (uint8_t[]){0x0F}, 1, 0},
    {0xae, (uint8_t[]){0x19}, 1, 0},
    {0xaf, (uint8_t[]){0x08}, 1, 0},
    {0xff, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 10}
};

typedef struct {
    board_desc_t base;

    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;

    adc_battery_monitor_t *battery_monitor;
    power_save_timer_t *pst;
    bool last_discharging;
} board_ctx_t;

static void on_boot_click(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) {
        board_enter_wifi_config_mode(board_get_instance());
        return;
    }
    app_toggle_chat(app);
}

static void on_charging_changed(bool is_charging, void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    power_save_timer_set_enabled(ctx->pst, !is_charging);
}

static void on_enter_sleep(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    display_set_power_save_mode(ctx->display, true);
    if (ctx->backlight) backlight_set_brightness(ctx->backlight, 1, false);
}

static void on_exit_sleep(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    display_set_power_save_mode(ctx->display, false);
    if (ctx->backlight) backlight_restore_brightness(ctx->backlight);
}

static void on_shutdown(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    ESP_LOGI(TAG, "Shutting down");
    rtc_gpio_set_level(GPIO_NUM_21, 0);
    rtc_gpio_hold_en(GPIO_NUM_21);
    esp_lcd_panel_disp_on_off(ctx->panel, false);
    esp_deep_sleep_start();
}

static void init_gpio(void)
{
    rtc_gpio_init(GPIO_NUM_21);
    rtc_gpio_set_direction(GPIO_NUM_21, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_NUM_21, 1);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_45),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(GPIO_NUM_45, 0);
}

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_SDA,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SCL,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_HEIGHT * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_nv3023_display(board_ctx_t *ctx)
{
    esp_lcd_panel_io_spi_config_t io_config = NV3023_PANEL_IO_SPI_CONFIG(DISPLAY_CS, DISPLAY_DC, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_config, &ctx->panel_io));

    nv3023_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RES,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_nv3023(ctx->panel_io, &panel_config, &ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(ctx->panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(ctx->panel, true));

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(board_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static void init_power(board_ctx_t *ctx)
{
    power_save_timer_cfg_t pst_cfg = {
        .cpu_max_freq = -1,
        .seconds_to_sleep = 60,
        .seconds_to_shutdown = 300,
    };
    ctx->pst = power_save_timer_create(&pst_cfg);
    power_save_timer_on_enter_sleep(ctx->pst, on_enter_sleep, ctx);
    power_save_timer_on_exit_sleep(ctx->pst, on_exit_sleep, ctx);
    power_save_timer_on_shutdown(ctx->pst, on_shutdown, ctx);
    power_save_timer_set_enabled(ctx->pst, true);

    adc_battery_monitor_cfg_t batt_cfg = {
        .adc_unit = ADC_UNIT_2,
        .adc_channel = ADC_CHANNEL_6,
        .upper_resistor = 100000,
        .lower_resistor = 100000,
        .charging_pin = GPIO_NUM_38,
    };
    ctx->battery_monitor = adc_battery_monitor_create(&batt_cfg);
    adc_battery_monitor_on_charging_changed(ctx->battery_monitor, on_charging_changed, ctx);
}

static const char *bd_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *bd_get_audio_codec(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = no_audio_codec_simplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
    }
    return ctx->codec;
}

static void *bd_get_display(board_desc_t *self)
{
    return ((board_ctx_t *)self)->display;
}

static void *bd_get_backlight(board_desc_t *self)
{
    return ((board_ctx_t *)self)->backlight;
}

static bool bd_get_battery_level(board_desc_t *self, int *level,
                                 bool *charging, bool *discharging)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    if (!ctx->battery_monitor) return false;
    *charging = adc_battery_monitor_is_charging(ctx->battery_monitor);
    *discharging = adc_battery_monitor_is_discharging(ctx->battery_monitor);
    if (*discharging != ctx->last_discharging) {
        power_save_timer_set_enabled(ctx->pst, *discharging);
        ctx->last_discharging = *discharging;
    }
    *level = adc_battery_monitor_get_level(ctx->battery_monitor);
    return true;
}

static void bd_destroy(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    if (ctx->boot_button) board_btn_delete(ctx->boot_button);
    if (ctx->pst) power_save_timer_destroy(ctx->pst);
    if (ctx->battery_monitor) adc_battery_monitor_delete(ctx->battery_monitor);
    if (ctx->backlight) backlight_destroy(ctx->backlight);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    board_ctx_t *ctx = calloc(1, sizeof(board_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = bd_get_board_type;
    ctx->base.get_audio_codec = bd_get_audio_codec;
    ctx->base.get_display = bd_get_display;
    ctx->base.get_backlight = bd_get_backlight;
    ctx->base.get_battery_level = bd_get_battery_level;
    ctx->base.destroy = bd_destroy;

    init_gpio();
    init_spi();
    init_nv3023_display(ctx);

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
                                          DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    init_power(ctx);
    init_buttons(ctx);

    backlight_restore_brightness(ctx->backlight);

    return &ctx->base;
}
