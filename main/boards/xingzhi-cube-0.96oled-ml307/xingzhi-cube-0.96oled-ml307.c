#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "audio/codecs/no_audio_codec.h"
#include "led/single_led.h"
#include "assets/lang_c.h"
#include "adc_battery_monitor.h"
#include "power_save_timer.h"
#include "device_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_i2c.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#define TAG "XINGZHI_CUBE_0_96OLED_ML307"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t display_i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;

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
}

static void on_exit_sleep(void *ud)
{
    board_ctx_t *ctx = (board_ctx_t *)ud;
    display_set_power_save_mode(ctx->display, false);
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

static void init_display_i2c(board_ctx_t *ctx)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = 0,
        .sda_io_num = DISPLAY_SDA_PIN,
        .scl_io_num = DISPLAY_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &ctx->display_i2c_bus));
}

static void init_ssd1306_display(board_ctx_t *ctx)
{
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = 0x3C,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 0,
        },
        .scl_speed_hz = 400 * 1000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(ctx->display_i2c_bus, &io_config, &ctx->panel_io));

    ESP_LOGI(TAG, "Install SSD1306 driver");
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = (uint8_t)DISPLAY_HEIGHT,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
        .vendor_config = &ssd1306_config,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(ctx->panel_io, &panel_config, &ctx->panel));
    ESP_LOGI(TAG, "SSD1306 driver installed");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(ctx->panel));
    if (esp_lcd_panel_init(ctx->panel) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display");
        ctx->display = no_display_create();
        return;
    }

    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(ctx->panel, true));

    ctx->display = oled_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
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

static void *bd_get_led(board_desc_t *self)
{
    board_ctx_t *ctx = (board_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
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
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    board_ctx_t *ctx = calloc(1, sizeof(board_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_DUAL;
    ctx->base.get_board_type = bd_get_board_type;
    ctx->base.get_led = bd_get_led;
    ctx->base.get_audio_codec = bd_get_audio_codec;
    ctx->base.get_display = bd_get_display;
    ctx->base.get_battery_level = bd_get_battery_level;
    ctx->base.destroy = bd_destroy;
    ctx->base.modem_tx_pin = ML307_TX_PIN;
    ctx->base.modem_rx_pin = ML307_RX_PIN;
    ctx->base.modem_dtr_pin = GPIO_NUM_NC;
    ctx->base.default_net_type = 1;

    init_power(ctx);
    init_display_i2c(ctx);
    init_ssd1306_display(ctx);
    init_buttons(ctx);

    return &ctx->base;
}
