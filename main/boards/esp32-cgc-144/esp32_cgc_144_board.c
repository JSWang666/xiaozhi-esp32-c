#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/mcp_server_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "audio/codecs/no_audio_codec.h"
#include "backlight.h"
#include "power_save_timer.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>
#include "c_api/board_c_api.h"

#ifndef DISPLAY_SPI_MODE
#define DISPLAY_SPI_MODE 0
#endif

#define TAG "ESP32_CGC_144"

#define BATTERY_ADC_INTERVAL 60
#define BATTERY_ADC_DATA_COUNT 3

typedef struct {
    board_desc_t base;

    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;
    power_save_timer_t *pst;

    board_btn_t *boot_button;
    board_btn_t *asr_button;

    bool lamp_power;
    gpio_num_t lamp_gpio;

    /* battery monitoring */
    gpio_num_t charging_pin;
    adc_oneshot_unit_handle_t adc_handle;
    esp_timer_handle_t battery_timer;
    uint16_t adc_values[BATTERY_ADC_DATA_COUNT];
    int adc_count;
    uint32_t battery_level;
    bool is_charging;
    int battery_tick;
} cgc144_ctx_t;

/* ── battery monitoring ────────────────────────────────────────────── */

static void read_battery_adc(cgc144_ctx_t *ctx)
{
    int adc_value;
    ESP_ERROR_CHECK(adc_oneshot_read(ctx->adc_handle, ADC_CHANNEL_3, &adc_value));

    if (ctx->adc_count < BATTERY_ADC_DATA_COUNT) {
        ctx->adc_values[ctx->adc_count++] = (uint16_t)adc_value;
    } else {
        for (int i = 0; i < BATTERY_ADC_DATA_COUNT - 1; i++)
            ctx->adc_values[i] = ctx->adc_values[i + 1];
        ctx->adc_values[BATTERY_ADC_DATA_COUNT - 1] = (uint16_t)adc_value;
    }

    uint32_t avg = 0;
    for (int i = 0; i < ctx->adc_count; i++) avg += ctx->adc_values[i];
    avg /= ctx->adc_count;

    static const struct { uint16_t adc; uint8_t level; } levels[] = {
        {1970, 0}, {2062, 20}, {2154, 40}, {2246, 60}, {2338, 80}, {2430, 100}
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

    ESP_LOGI(TAG, "ADC value: %d average: %lu level: %lu",
             adc_value, (unsigned long)avg, (unsigned long)ctx->battery_level);
}

static void battery_timer_cb(void *arg)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)arg;
    bool new_charging = gpio_get_level(ctx->charging_pin) == 1;
    if (new_charging != ctx->is_charging) {
        ctx->is_charging = new_charging;
        if (ctx->pst) {
            power_save_timer_set_enabled(ctx->pst, !ctx->is_charging);
        }
        read_battery_adc(ctx);
        return;
    }

    if (ctx->adc_count < BATTERY_ADC_DATA_COUNT) {
        read_battery_adc(ctx);
        return;
    }

    ctx->battery_tick++;
    if (ctx->battery_tick % BATTERY_ADC_INTERVAL == 0) {
        read_battery_adc(ctx);
    }
}

static void init_power_manager(cgc144_ctx_t *ctx)
{
#if defined(ESP32_CGC_144_lite)
    ctx->charging_pin = GPIO_NUM_NC;
#else
    ctx->charging_pin = GPIO_NUM_36;
#endif

    if (ctx->charging_pin != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << ctx->charging_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &ctx->adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(ctx->adc_handle, ADC_CHANNEL_3, &chan_cfg));

    esp_timer_create_args_t timer_args = {
        .callback = battery_timer_cb,
        .arg = ctx,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "battery_check_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &ctx->battery_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(ctx->battery_timer, 1000000));
}

/* ── power save callbacks ──────────────────────────────────────────── */

static void on_enter_sleep(void *ud)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)ud;
    if (ctx->display)
        display_set_power_save_mode(ctx->display, true);
    if (ctx->backlight)
        backlight_set_brightness(ctx->backlight, 1, false);
}

static void on_exit_sleep(void *ud)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)ud;
    if (ctx->display)
        display_set_power_save_mode(ctx->display, false);
    if (ctx->backlight)
        backlight_restore_brightness(ctx->backlight);
}

static void init_power_save_timer(cgc144_ctx_t *ctx)
{
    power_save_timer_cfg_t cfg = {
        .cpu_max_freq = -1,
        .seconds_to_sleep = 60,
        .seconds_to_shutdown = -1,
    };
    ctx->pst = power_save_timer_create(&cfg);
    power_save_timer_on_enter_sleep(ctx->pst, on_enter_sleep, ctx);
    power_save_timer_on_exit_sleep(ctx->pst, on_exit_sleep, ctx);
    power_save_timer_set_enabled(ctx->pst, true);
}

/* ── buttons ───────────────────────────────────────────────────────── */

static void on_boot_click(void *ud)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)ud;
    if (ctx->pst) power_save_timer_wake_up(ctx->pst);
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) {
        board_enter_wifi_config_mode(board_get_instance());
        return;
    }
    app_toggle_chat(app);
}

static void on_asr_click(void *ud)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)ud;
    if (ctx->pst) power_save_timer_wake_up(ctx->pst);
    app_context_t *app = app_get_context();
    if (app) app_wake_word_invoke(app, "\xe4\xbd\xa0\xe5\xa5\xbd\xe5\xb0\x8f\xe6\x99\xba");
}

/* ── lamp MCP tools ────────────────────────────────────────────────── */

static mcp_tool_result_t lamp_get_state(const void *args, void *ud)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    res.text = strdup(ctx->lamp_power ? "{\"power\": true}" : "{\"power\": false}");
    return res;
}

static mcp_tool_result_t lamp_turn_on(const void *args, void *ud)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)ud;
    ctx->lamp_power = true;
    gpio_set_level(ctx->lamp_gpio, 1);
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t lamp_turn_off(const void *args, void *ud)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)ud;
    ctx->lamp_power = false;
    gpio_set_level(ctx->lamp_gpio, 0);
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

/* ── init helpers ──────────────────────────────────────────────────── */

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SCLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_lcd_display(cgc144_ctx_t *ctx)
{
    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = DISPLAY_SPI_MODE,
        .pclk_hz = DISPLAY_SPI_SCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &ctx->panel_io));

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RESET_PIN,
        .rgb_ele_order = DISPLAY_RGB_ORDER,
        .bits_per_pixel = 16,
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

static void init_buttons(cgc144_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t asr_cfg = { .gpio_num = ASR_BUTTON_GPIO };
    ctx->asr_button = board_btn_create_gpio(&asr_cfg);
    board_btn_on_click(ctx->asr_button, on_asr_click, ctx);
}

static void init_lamp(cgc144_ctx_t *ctx)
{
    ctx->lamp_gpio = LAMP_GPIO;
    if (ctx->lamp_gpio == GPIO_NUM_NC) return;

    gpio_config_t config = {
        .pin_bit_mask = (1ULL << ctx->lamp_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_set_level(ctx->lamp_gpio, 0);

    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return;

    mcp_server_add_tool_c(mcp, "self.lamp.get_state",
        "Get the power state of the lamp", NULL, 0, lamp_get_state, ctx);
    mcp_server_add_tool_c(mcp, "self.lamp.turn_on",
        "Turn on the lamp", NULL, 0, lamp_turn_on, ctx);
    mcp_server_add_tool_c(mcp, "self.lamp.turn_off",
        "Turn off the lamp", NULL, 0, lamp_turn_off, ctx);
}

/* ── board_desc_t vtable ───────────────────────────────────────────── */

static const char *cgc144_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *cgc144_get_audio_codec(board_desc_t *self)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = no_audio_codec_simplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
    }
    return ctx->codec;
}

static void *cgc144_get_display(board_desc_t *self)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)self;
    return ctx->display;
}

static void *cgc144_get_backlight(board_desc_t *self)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static bool cgc144_get_battery_level(board_desc_t *self, int *level,
                                     bool *charging, bool *discharging)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)self;
    *charging = (ctx->battery_level < 100) && ctx->is_charging;
    *discharging = !ctx->is_charging;
    *level = (int)ctx->battery_level;
    return true;
}

static void cgc144_destroy(board_desc_t *self)
{
    cgc144_ctx_t *ctx = (cgc144_ctx_t *)self;
    if (ctx->battery_timer) {
        esp_timer_stop(ctx->battery_timer);
        esp_timer_delete(ctx->battery_timer);
    }
    if (ctx->adc_handle) adc_oneshot_del_unit(ctx->adc_handle);
    if (ctx->pst) power_save_timer_destroy(ctx->pst);
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->asr_button);
    if (ctx->backlight) backlight_destroy(ctx->backlight);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    cgc144_ctx_t *ctx = calloc(1, sizeof(cgc144_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = cgc144_get_board_type;
    ctx->base.get_audio_codec = cgc144_get_audio_codec;
    ctx->base.get_display = cgc144_get_display;
    ctx->base.get_backlight = cgc144_get_backlight;
    ctx->base.get_battery_level = cgc144_get_battery_level;
    ctx->base.destroy = cgc144_destroy;

    init_power_manager(ctx);
    init_power_save_timer(ctx);
    init_spi();
    init_lcd_display(ctx);
    init_buttons(ctx);
    init_lamp(ctx);

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
        DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    backlight_restore_brightness(ctx->backlight);

    return &ctx->base;
}
