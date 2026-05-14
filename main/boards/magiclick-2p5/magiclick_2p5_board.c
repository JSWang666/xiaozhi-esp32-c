#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "backlight.h"
#include "power_save_timer.h"
#include "device_state.h"
#include "assets/lang_c.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_gc9a01.h>

#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_timer.h>

#define TAG "magiclick_2p5"

#define CHARGING_PIN          GPIO_NUM_48
#define CHARGING_ACTIVE_STATE 0
#define BATTERY_ADC_CHANNEL   ADC_CHANNEL_6
#define BATTERY_ADC_COUNT     5
#define BATTERY_CHECK_INTERVAL 60

extern led_t *circular_strip_led_create(int gpio, int max_leds);

static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
     (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                 0x04, 0x12, 0x14, 0x1f},
     14, 0},
    {0xf1,
     (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                 0x0C, 0x1A, 0x14, 0x1E},
     14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};

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

    board_btn_t *main_button;
    board_btn_t *left_button;
    board_btn_t *right_button;

    /* battery / power */
    esp_timer_handle_t battery_timer;
    adc_oneshot_unit_handle_t adc_handle;
    uint16_t adc_values[BATTERY_ADC_COUNT];
    int adc_count;
    int adc_write_idx;
    uint32_t battery_level;
    bool is_charging;
    int battery_tick;

    uint8_t pcb_version;
} magiclick_2p5_ctx_t;

/* ── battery monitoring ────────────────────────────────────────────── */

static void read_battery_adc(magiclick_2p5_ctx_t *ctx)
{
    int raw;
    ESP_ERROR_CHECK(adc_oneshot_read(ctx->adc_handle, BATTERY_ADC_CHANNEL, &raw));

    ctx->adc_values[ctx->adc_write_idx] = (uint16_t)raw;
    ctx->adc_write_idx = (ctx->adc_write_idx + 1) % BATTERY_ADC_COUNT;
    if (ctx->adc_count < BATTERY_ADC_COUNT) ctx->adc_count++;

    uint32_t avg = 0;
    for (int i = 0; i < ctx->adc_count; i++) avg += ctx->adc_values[i];
    avg /= ctx->adc_count;

    static const struct { uint16_t adc; uint8_t level; } levels[] = {
        {1985, 0}, {2079, 20}, {2141, 40}, {2296, 60}, {2420, 80}, {2606, 100}
    };

    if (avg < levels[0].adc) {
        ctx->battery_level = 0;
    } else if (avg >= levels[5].adc) {
        ctx->battery_level = 100;
    } else {
        for (int i = 0; i < 5; i++) {
            if (avg >= levels[i].adc && avg < levels[i + 1].adc) {
                float ratio = (float)(avg - levels[i].adc) /
                              (levels[i + 1].adc - levels[i].adc);
                ctx->battery_level = levels[i].level +
                    (uint32_t)(ratio * (levels[i + 1].level - levels[i].level));
                break;
            }
        }
    }
}

static void battery_timer_cb(void *arg)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)arg;
    bool charging = (gpio_get_level(CHARGING_PIN) == CHARGING_ACTIVE_STATE);
    if (charging != ctx->is_charging) {
        ctx->is_charging = charging;
        power_save_timer_set_enabled(ctx->pst, !charging);
        read_battery_adc(ctx);
        return;
    }
    if (ctx->adc_count < BATTERY_ADC_COUNT) {
        read_battery_adc(ctx);
        return;
    }
    ctx->battery_tick++;
    if (ctx->battery_tick % BATTERY_CHECK_INTERVAL == 0) {
        read_battery_adc(ctx);
    }
}

static void init_battery(magiclick_2p5_ctx_t *ctx)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CHARGING_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    adc_oneshot_unit_init_cfg_t adc_init = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init, &ctx->adc_handle));
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(ctx->adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg));

    esp_timer_create_args_t timer_args = {
        .callback = battery_timer_cb,
        .arg = ctx,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "battery_check",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &ctx->battery_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(ctx->battery_timer, 1000000));
}

/* ── PCB version detection via ADC ─────────────────────────────────── */

static void check_pcb_version(magiclick_2p5_ctx_t *ctx)
{
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_2, &config));

    int adc_value = 0;
    int raw_value;
    for (int i = 0; i < 10; i++) {
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_2, &raw_value));
        adc_value += raw_value;
    }
    adc_value /= 10;

    int voltage = (int)(adc_value * (3300 / 4095.0));
    if (voltage < 100) {
        ctx->pcb_version = PCB_VERSION_2_5A;
    } else if (voltage > 3200) {
        ctx->pcb_version = PCB_VERSION_2_5A1;
    }

    adc_oneshot_del_unit(adc1_handle);
    ESP_LOGI(TAG, "io voltage: %d, pcb_version: %d", voltage, ctx->pcb_version);
}

/* ── 4G module control ─────────────────────────────────────────────── */

static void enable_4g_module(void)
{
    gpio_reset_pin(ML307_POWER_PIN);
    gpio_set_direction(ML307_POWER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ML307_POWER_PIN, ML307_POWER_OUTPUT_INVERT ? 0 : 1);
}

static void disable_4g_module(void)
{
    gpio_reset_pin(ML307_POWER_PIN);
    gpio_set_direction(ML307_POWER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ML307_POWER_PIN, ML307_POWER_OUTPUT_INVERT ? 1 : 0);
}

/* ── power-save callbacks ──────────────────────────────────────────── */

static void on_enter_sleep(void *ud)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)ud;
    display_set_power_save_mode(ctx->display, true);
    backlight_set_brightness(ctx->backlight, 1, false);
}

static void on_exit_sleep(void *ud)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)ud;
    display_set_power_save_mode(ctx->display, false);
    backlight_restore_brightness(ctx->backlight);
}

/* ── button callbacks ──────────────────────────────────────────────── */

static void on_main_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) {
        return;
    }
}

static void on_main_press_down(void *ud)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    app_context_t *app = app_get_context();
    if (app) app_start_listening(app);
}

static void on_main_press_up(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_stop_listening(app);
}

static void on_left_click(void *ud)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume - 10;
    if (vol < 0) vol = 0;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    display_show_notification(ctx->display, buf, 0);
}

static void on_left_long(void *ud)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
    display_show_notification(ctx->display, lang_str_muted, 0);
}

static void on_right_click(void *ud)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume + 10;
    if (vol > 100) vol = 100;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, vol);
    display_show_notification(ctx->display, buf, 0);
}

static void on_right_long(void *ud)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)ud;
    power_save_timer_wake_up(ctx->pst);
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
    display_show_notification(ctx->display, lang_str_max_volume, 0);
}

/* ── init helpers ──────────────────────────────────────────────────── */

static void init_led_power(void)
{
    gpio_reset_pin(BUILTIN_LED_POWER);
    gpio_set_direction(BUILTIN_LED_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(BUILTIN_LED_POWER, BUILTIN_LED_POWER_OUTPUT_INVERT ? 0 : 1);
}

static void init_codec_i2c(magiclick_2p5_ctx_t *ctx)
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
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_lcd_display(magiclick_2p5_ctx_t *ctx)
{
    bool use_gc9107;
    bool mirror_x, mirror_y, swap_xy, invert_color;
    lcd_rgb_element_order_t rgb_order;
    int offset_x, offset_y, spi_mode;
    const char *screen_name;

    if (ctx->pcb_version == PCB_VERSION_2_5A) {
        use_gc9107 = true;
        mirror_x = false; mirror_y = false; swap_xy = false;
        invert_color = false;
        rgb_order = LCD_RGB_ELEMENT_ORDER_RGB;
        offset_x = 0; offset_y = 0; spi_mode = 0;
        screen_name = "GC9107";
    } else {
        use_gc9107 = false;
        mirror_x = true; mirror_y = true; swap_xy = false;
        invert_color = true;
        rgb_order = LCD_RGB_ELEMENT_ORDER_BGR;
        offset_x = 2; offset_y = 3; spi_mode = 0;
        screen_name = (ctx->pcb_version == PCB_VERSION_2_5A1)
                          ? "ST7735" : "ST7735 (default)";
    }

    ESP_LOGW(TAG, "PCB Version: %d, Using %s screen", ctx->pcb_version, screen_name);

    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = spi_mode,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &ctx->panel_io));

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RST_PIN,
        .rgb_ele_order = rgb_order,
        .bits_per_pixel = 16,
    };

    if (use_gc9107) {
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };
        panel_config.vendor_config = &gc9107_vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(ctx->panel_io, &panel_config, &ctx->panel));
    } else {
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel));
    }

    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, invert_color);
    esp_lcd_panel_swap_xy(ctx->panel, swap_xy);
    esp_lcd_panel_mirror(ctx->panel, mirror_x, mirror_y);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(ctx->panel, true));

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, offset_x, offset_y,
        mirror_x, mirror_y, swap_xy);
}

static void init_buttons(magiclick_2p5_ctx_t *ctx)
{
    board_btn_gpio_cfg_t main_cfg = { .gpio_num = MAIN_BUTTON_GPIO };
    ctx->main_button = board_btn_create_gpio(&main_cfg);
    board_btn_on_click(ctx->main_button, on_main_click, ctx);
    board_btn_on_press_down(ctx->main_button, on_main_press_down, ctx);
    board_btn_on_press_up(ctx->main_button, on_main_press_up, ctx);

    board_btn_gpio_cfg_t left_cfg = { .gpio_num = LEFT_BUTTON_GPIO };
    ctx->left_button = board_btn_create_gpio(&left_cfg);
    board_btn_on_click(ctx->left_button, on_left_click, ctx);
    board_btn_on_long_press(ctx->left_button, on_left_long, ctx);

    board_btn_gpio_cfg_t right_cfg = { .gpio_num = RIGHT_BUTTON_GPIO };
    ctx->right_button = board_btn_create_gpio(&right_cfg);
    board_btn_on_click(ctx->right_button, on_right_click, ctx);
    board_btn_on_long_press(ctx->right_button, on_right_long, ctx);
}

static void init_power_save(magiclick_2p5_ctx_t *ctx)
{
    power_save_timer_cfg_t cfg = {
        .cpu_max_freq = 240,
        .seconds_to_sleep = 60,
        .seconds_to_shutdown = -1,
    };
    ctx->pst = power_save_timer_create(&cfg);
    power_save_timer_on_enter_sleep(ctx->pst, on_enter_sleep, ctx);
    power_save_timer_on_exit_sleep(ctx->pst, on_exit_sleep, ctx);
    power_save_timer_set_enabled(ctx->pst, true);
}

/* ── board_desc_t vtable ───────────────────────────────────────────── */

static const char *m2p5_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *m2p5_get_led(board_desc_t *self)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = circular_strip_led_create(BUILTIN_LED_GPIO, BUILTIN_LED_NUM);
    }
    return ctx->led;
}

static void *m2p5_get_audio_codec(board_desc_t *self)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->codec_i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, false);
    }
    return ctx->codec;
}

static void *m2p5_get_display(board_desc_t *self)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)self;
    return ctx->display;
}

static void *m2p5_get_backlight(board_desc_t *self)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)self;
    return ctx->backlight;
}

static bool m2p5_get_battery_level(board_desc_t *self, int *level,
                                   bool *charging, bool *discharging)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)self;
    static bool last_discharging = false;
    *charging = ctx->is_charging;
    *discharging = !ctx->is_charging;
    if (*discharging != last_discharging) {
        power_save_timer_set_enabled(ctx->pst, *discharging);
        last_discharging = *discharging;
    }
    *level = (int)ctx->battery_level;
    return true;
}

static void m2p5_destroy(board_desc_t *self)
{
    magiclick_2p5_ctx_t *ctx = (magiclick_2p5_ctx_t *)self;
    if (ctx->backlight) {
        backlight_destroy(ctx->backlight);
        ctx->backlight = NULL;
    }
    if (ctx->battery_timer) {
        esp_timer_stop(ctx->battery_timer);
        esp_timer_delete(ctx->battery_timer);
    }
    if (ctx->adc_handle) adc_oneshot_del_unit(ctx->adc_handle);
    board_btn_delete(ctx->main_button);
    board_btn_delete(ctx->left_button);
    board_btn_delete(ctx->right_button);
    if (ctx->pst) power_save_timer_destroy(ctx->pst);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    magiclick_2p5_ctx_t *ctx = calloc(1, sizeof(magiclick_2p5_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_DUAL;
    ctx->base.get_board_type = m2p5_get_board_type;
    ctx->base.get_led = m2p5_get_led;
    ctx->base.get_audio_codec = m2p5_get_audio_codec;
    ctx->base.get_display = m2p5_get_display;
    ctx->base.get_backlight = m2p5_get_backlight;
    ctx->base.get_battery_level = m2p5_get_battery_level;
    ctx->base.destroy = m2p5_destroy;
    ctx->base.modem_tx_pin = ML307_TX_PIN;
    ctx->base.modem_rx_pin = ML307_RX_PIN;
    ctx->base.modem_dtr_pin = GPIO_NUM_NC;
    ctx->base.default_net_type = 1;

    check_pcb_version(ctx);
    init_led_power();

    disable_4g_module();

    init_battery(ctx);
    init_codec_i2c(ctx);
    init_spi();
    init_lcd_display(ctx);

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
        DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);

    init_buttons(ctx);
    init_power_save(ctx);
    backlight_restore_brightness(ctx->backlight);

    return &ctx->base;
}
