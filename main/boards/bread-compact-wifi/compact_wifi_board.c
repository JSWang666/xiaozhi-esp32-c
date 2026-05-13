#include "board_defs.h"
#include "button.h"
#include "system_reset.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/board_c_api.h"
#include "c_api/mcp_server_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "audio/codecs/no_audio_codec.h"
#include "assets/lang_c.h"
#include "device_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_i2c.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t display_i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;

    board_btn_t *boot_button;
    board_btn_t *touch_button;
    board_btn_t *volume_up_button;
    board_btn_t *volume_down_button;

    bool lamp_power;
    gpio_num_t lamp_gpio;
} compact_wifi_ctx_t;

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

static void on_touch_press_down(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_start_listening(app);
}

static void on_touch_press_up(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_stop_listening(app);
}

static void on_volume_up_click(void *ud)
{
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)ud;
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
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)ud;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_max_volume, 0);
}

static void on_volume_down_click(void *ud)
{
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)ud;
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
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)ud;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
    if (ctx->display)
        display_show_notification(ctx->display, lang_str_muted, 0);
}

static void init_display_i2c(compact_wifi_ctx_t *ctx)
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

static void init_ssd1306_display(compact_wifi_ctx_t *ctx)
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
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
    };

    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = (uint8_t)DISPLAY_HEIGHT,
    };
    panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(ctx->panel_io, &panel_config, &ctx->panel));
#else
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(ctx->panel_io, &panel_config, &ctx->panel));
#endif
    ESP_LOGI(TAG, "SSD1306 driver installed");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(ctx->panel));
    if (esp_lcd_panel_init(ctx->panel) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display");
        ctx->display = no_display_create();
        return;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(ctx->panel, false));

    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(ctx->panel, true));

    ctx->display = oled_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
}

static void init_buttons(compact_wifi_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t touch_cfg = { .gpio_num = TOUCH_BUTTON_GPIO };
    ctx->touch_button = board_btn_create_gpio(&touch_cfg);
    board_btn_on_press_down(ctx->touch_button, on_touch_press_down, ctx);
    board_btn_on_press_up(ctx->touch_button, on_touch_press_up, ctx);

    board_btn_gpio_cfg_t vol_up_cfg = { .gpio_num = VOLUME_UP_BUTTON_GPIO };
    ctx->volume_up_button = board_btn_create_gpio(&vol_up_cfg);
    board_btn_on_click(ctx->volume_up_button, on_volume_up_click, ctx);
    board_btn_on_long_press(ctx->volume_up_button, on_volume_up_long, ctx);

    board_btn_gpio_cfg_t vol_down_cfg = { .gpio_num = VOLUME_DOWN_BUTTON_GPIO };
    ctx->volume_down_button = board_btn_create_gpio(&vol_down_cfg);
    board_btn_on_click(ctx->volume_down_button, on_volume_down_click, ctx);
    board_btn_on_long_press(ctx->volume_down_button, on_volume_down_long, ctx);
}

static mcp_tool_result_t lamp_get_state(const void *args, void *ud)
{
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    res.text = strdup(ctx->lamp_power ? "{\"power\": true}" : "{\"power\": false}");
    return res;
}

static mcp_tool_result_t lamp_turn_on(const void *args, void *ud)
{
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)ud;
    ctx->lamp_power = true;
    gpio_set_level(ctx->lamp_gpio, 1);
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t lamp_turn_off(const void *args, void *ud)
{
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)ud;
    ctx->lamp_power = false;
    gpio_set_level(ctx->lamp_gpio, 0);
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static void init_tools(compact_wifi_ctx_t *ctx)
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

static const char *cwb_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *cwb_get_led(board_desc_t *self)
{
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *cwb_get_audio_codec(board_desc_t *self)
{
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)self;
    if (!ctx->codec) {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        ctx->codec = no_audio_codec_simplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        ctx->codec = no_audio_codec_duplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
    }
    return ctx->codec;
}

static void *cwb_get_display(board_desc_t *self)
{
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)self;
    return ctx->display;
}

static void cwb_destroy(board_desc_t *self)
{
    compact_wifi_ctx_t *ctx = (compact_wifi_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->touch_button);
    board_btn_delete(ctx->volume_up_button);
    board_btn_delete(ctx->volume_down_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    compact_wifi_ctx_t *ctx = calloc(1, sizeof(compact_wifi_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = cwb_get_board_type;
    ctx->base.get_led = cwb_get_led;
    ctx->base.get_audio_codec = cwb_get_audio_codec;
    ctx->base.get_display = cwb_get_display;
    ctx->base.destroy = cwb_destroy;

    init_display_i2c(ctx);
    init_ssd1306_display(ctx);
    init_buttons(ctx);
    init_tools(ctx);

    return &ctx->base;
}
