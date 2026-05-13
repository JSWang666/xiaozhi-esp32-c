#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "assets/lang_c.h"
#include "device_state.h"
#include "axp2101.h"
#include "i2c_device.h"
#include "power_save_timer.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_i2c.h>

#define TAG "KevinBoxBoard"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t display_i2c_bus;
    i2c_master_bus_handle_t codec_i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    axp2101_t *pmic;
    power_save_timer_t *pst;

    board_btn_t *boot_button;
    board_btn_t *volume_up_button;
    board_btn_t *volume_down_button;
} kevin_box2_ctx_t;

static void init_pmic(kevin_box2_ctx_t *ctx)
{
    ctx->pmic = axp2101_create(ctx->codec_i2c_bus, AXP2101_I2C_ADDR);
    i2c_device_t *dev = axp2101_get_i2c_device(ctx->pmic);

    i2c_device_write_reg(dev, 0x22, 0x06);  /* PWRON > OFFLEVEL as POWEROFF Source enable */
    i2c_device_write_reg(dev, 0x27, 0x10);  /* hold 4s to power off */
    i2c_device_write_reg(dev, 0x93, 0x1C);  /* ALDO2 output 3.3V */

    uint8_t val = i2c_device_read_reg(dev, 0x90);
    val |= 0x02;
    i2c_device_write_reg(dev, 0x90, val);   /* enable ALDO2 */

    i2c_device_write_reg(dev, 0x64, 0x03);  /* CV charger voltage 4.2V */
    i2c_device_write_reg(dev, 0x61, 0x05);  /* precharge current 125mA */
    i2c_device_write_reg(dev, 0x62, 0x0A);  /* charger current 400mA */
    i2c_device_write_reg(dev, 0x63, 0x15);  /* term charge current 125mA */
    i2c_device_write_reg(dev, 0x14, 0x00);  /* min system voltage 4.1V */
    i2c_device_write_reg(dev, 0x15, 0x00);  /* input voltage limit 3.88V */
    i2c_device_write_reg(dev, 0x16, 0x05);  /* input current limit 2000mA */
    i2c_device_write_reg(dev, 0x24, 0x01);  /* Vsys PWROFF threshold 3.2V */
    i2c_device_write_reg(dev, 0x50, 0x14);  /* TS pin as EXTERNAL input */
}

static void enable_4g_module(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << 4),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(GPIO_NUM_4, 1);
}

static void init_display_i2c(kevin_box2_ctx_t *ctx)
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

static void init_ssd1306_display(kevin_box2_ctx_t *ctx)
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
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(ctx->display_i2c_bus,
                                                 &io_config, &ctx->panel_io));

    ESP_LOGI(TAG, "Install SSD1306 driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
    };
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = (uint8_t)DISPLAY_HEIGHT,
    };
    panel_config.vendor_config = &ssd1306_config;

    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(ctx->panel_io,
                                               &panel_config, &ctx->panel));
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

static void init_codec_i2c(kevin_box2_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = 1,
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

/* ── button callbacks ─────────────────────────────────────────────── */

static void on_boot_press_down(void *ud)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)ud;
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

static void on_volume_up_click(void *ud)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume + 10;
    if (vol > 100) vol = 100;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display)
        display_show_notification(ctx->display, buf, 0);
}

static void on_volume_up_long(void *ud)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_max_volume, 0);
}

static void on_volume_down_click(void *ud)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume - 10;
    if (vol < 0) vol = 0;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    if (ctx->display)
        display_show_notification(ctx->display, buf, 0);
}

static void on_volume_down_long(void *ud)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_muted, 0);
}

static void init_buttons(kevin_box2_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_press_down(ctx->boot_button, on_boot_press_down, ctx);
    board_btn_on_press_up(ctx->boot_button, on_boot_press_up, ctx);

    board_btn_gpio_cfg_t vol_up_cfg = { .gpio_num = VOLUME_UP_BUTTON_GPIO };
    ctx->volume_up_button = board_btn_create_gpio(&vol_up_cfg);
    board_btn_on_click(ctx->volume_up_button, on_volume_up_click, ctx);
    board_btn_on_long_press(ctx->volume_up_button, on_volume_up_long, ctx);

    board_btn_gpio_cfg_t vol_down_cfg = { .gpio_num = VOLUME_DOWN_BUTTON_GPIO };
    ctx->volume_down_button = board_btn_create_gpio(&vol_down_cfg);
    board_btn_on_click(ctx->volume_down_button, on_volume_down_click, ctx);
    board_btn_on_long_press(ctx->volume_down_button, on_volume_down_long, ctx);
}

/* ── power save ───────────────────────────────────────────────────── */

static void on_shutdown_request(void *ud)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)ud;
    axp2101_power_off(ctx->pmic);
}

static void init_power_save(kevin_box2_ctx_t *ctx)
{
    power_save_timer_cfg_t cfg = {
        .cpu_max_freq = -1,
        .seconds_to_sleep = -1,
        .seconds_to_shutdown = 600,
    };
    ctx->pst = power_save_timer_create(&cfg);
    power_save_timer_on_shutdown(ctx->pst, on_shutdown_request, ctx);
    power_save_timer_set_enabled(ctx->pst, true);
}

/* ── board_desc vtable ────────────────────────────────────────────── */

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_led(board_desc_t *self)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)self;
    if (!ctx->led)
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = box_codec_create(
            ctx->codec_i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)self;
    return ctx->display;
}

static bool get_battery_level(board_desc_t *self, int *level,
                              bool *charging, bool *discharging)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)self;
    static bool last_discharging = false;

    *charging = axp2101_is_charging(ctx->pmic);
    *discharging = axp2101_is_discharging(ctx->pmic);

    if (*discharging != last_discharging) {
        power_save_timer_set_enabled(ctx->pst, *discharging);
        last_discharging = *discharging;
    }

    *level = axp2101_get_battery_level(ctx->pmic);
    return true;
}

static void destroy(board_desc_t *self)
{
    kevin_box2_ctx_t *ctx = (kevin_box2_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->volume_up_button);
    board_btn_delete(ctx->volume_down_button);
    if (ctx->pst) power_save_timer_destroy(ctx->pst);
    if (ctx->pmic) axp2101_destroy(ctx->pmic);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    kevin_box2_ctx_t *ctx = calloc(1, sizeof(kevin_box2_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_DUAL;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_battery_level = get_battery_level;
    ctx->base.destroy = destroy;

    init_display_i2c(ctx);
    init_ssd1306_display(ctx);
    init_codec_i2c(ctx);
    init_pmic(ctx);
    enable_4g_module();
    init_buttons(ctx);
    init_power_save(ctx);

    return &ctx->base;
}
