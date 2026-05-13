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
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_spi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#define TAG "XINGZHI_CUBE_1_54TFT_ML307"

typedef struct {
    board_desc_t base;

    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
    board_btn_t *volume_up_button;
    board_btn_t *volume_down_button;

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
    app_toggle_chat(app);
}

static void on_volume_up_click(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume + 10;
    if (vol > 100) vol = 100;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display) display_show_notification(ctx->display, buf, 0);
}

static void on_volume_up_long(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
    if (ctx->display) display_show_notification(ctx->display, lang_str_max_volume, 0);
}

static void on_volume_down_click(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume - 10;
    if (vol < 0) vol = 0;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display) display_show_notification(ctx->display, buf, 0);
}

static void on_volume_down_long(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
    if (ctx->display) display_show_notification(ctx->display, lang_str_muted, 0);
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

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_SDA,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SCL,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_st7789_display(board_ctx_t *ctx)
{
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS,
        .dc_gpio_num = DISPLAY_DC,
        .spi_mode = 3,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &ctx->panel_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RES,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(ctx->panel, true));

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(board_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t vol_up_cfg = { .gpio_num = VOLUME_UP_BUTTON_GPIO };
    ctx->volume_up_button = board_btn_create_gpio(&vol_up_cfg);
    board_btn_on_click(ctx->volume_up_button, on_volume_up_click, ctx);
    board_btn_on_long_press(ctx->volume_up_button, on_volume_up_long, ctx);

    board_btn_gpio_cfg_t vol_down_cfg = { .gpio_num = VOLUME_DOWN_BUTTON_GPIO };
    ctx->volume_down_button = board_btn_create_gpio(&vol_down_cfg);
    board_btn_on_click(ctx->volume_down_button, on_volume_down_click, ctx);
    board_btn_on_long_press(ctx->volume_down_button, on_volume_down_long, ctx);
}

static void init_power(board_ctx_t *ctx)
{
    rtc_gpio_init(GPIO_NUM_21);
    rtc_gpio_set_direction(GPIO_NUM_21, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_NUM_21, 1);

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
    if (ctx->volume_up_button) board_btn_delete(ctx->volume_up_button);
    if (ctx->volume_down_button) board_btn_delete(ctx->volume_down_button);
    if (ctx->pst) power_save_timer_destroy(ctx->pst);
    if (ctx->battery_monitor) adc_battery_monitor_delete(ctx->battery_monitor);
    if (ctx->backlight) backlight_destroy(ctx->backlight);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    board_ctx_t *ctx = calloc(1, sizeof(board_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_DUAL;
    ctx->base.get_board_type = bd_get_board_type;
    ctx->base.get_audio_codec = bd_get_audio_codec;
    ctx->base.get_display = bd_get_display;
    ctx->base.get_backlight = bd_get_backlight;
    ctx->base.get_battery_level = bd_get_battery_level;
    ctx->base.destroy = bd_destroy;

    init_spi();
    init_st7789_display(ctx);

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
                                          DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    init_power(ctx);
    init_buttons(ctx);

    backlight_restore_brightness(ctx->backlight);

    return &ctx->base;
}
