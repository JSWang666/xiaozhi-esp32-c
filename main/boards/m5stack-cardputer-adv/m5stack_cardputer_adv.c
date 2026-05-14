#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "c_api/codec_c_api.h"
#include "backlight.h"
#include "device_state.h"
#include "display/display.h"
#include "assets/lang_c.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_common.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_manager.h>
#include <ssid_manager.h>
#include "c_api/board_c_api.h"

/* --- Keyboard C types (from tca8418_keyboard module) --- */
enum {
    KC_NONE = 0x00,
    KC_A = 0x04, KC_B = 0x05, KC_C = 0x06, KC_D = 0x07,
    KC_E = 0x08, KC_F = 0x09, KC_G = 0x0A, KC_H = 0x0B,
    KC_I = 0x0C, KC_J = 0x0D, KC_K = 0x0E, KC_L = 0x0F,
    KC_M = 0x10, KC_N = 0x11, KC_O = 0x12, KC_P = 0x13,
    KC_Q = 0x14, KC_R = 0x15, KC_S = 0x16, KC_T = 0x17,
    KC_U = 0x18, KC_V = 0x19, KC_W = 0x1A, KC_X = 0x1B,
    KC_Y = 0x1C, KC_Z = 0x1D,
    KC_1 = 0x1E, KC_2 = 0x1F, KC_3 = 0x20, KC_4 = 0x21,
    KC_5 = 0x22, KC_6 = 0x23, KC_7 = 0x24, KC_8 = 0x25,
    KC_9 = 0x26, KC_0 = 0x27,
    KC_ENTER = 0x28, KC_ESC = 0x29, KC_BACKSPACE = 0x2A,
    KC_TAB = 0x2B, KC_SPACE = 0x2C,
    KC_RIGHT = 0x4F, KC_LEFT = 0x50, KC_DOWN = 0x51, KC_UP = 0x52,
};

typedef enum {
    TCA8418_KEY_NONE = 0,
    TCA8418_KEY_UP, TCA8418_KEY_DOWN,
    TCA8418_KEY_LEFT, TCA8418_KEY_RIGHT,
    TCA8418_KEY_ENTER, TCA8418_KEY_OTHER
} tca8418_legacy_key_t;

typedef struct {
    bool pressed;
    bool is_modifier;
    uint8_t key_code;
    const char *key_char;
} tca8418_key_event_t;

typedef struct tca8418_keyboard tca8418_keyboard_t;
typedef void (*tca8418_key_cb_t)(tca8418_legacy_key_t key, void *ud);
typedef void (*tca8418_key_event_cb_t)(const tca8418_key_event_t *event, void *ud);

tca8418_keyboard_t *tca8418_keyboard_create(i2c_master_bus_handle_t i2c_bus, uint8_t addr, gpio_num_t int_pin);
void tca8418_keyboard_destroy(tca8418_keyboard_t *kb);
void tca8418_keyboard_initialize(tca8418_keyboard_t *kb);
void tca8418_keyboard_set_key_cb(tca8418_keyboard_t *kb, tca8418_key_cb_t cb, void *ud);
void tca8418_keyboard_set_key_event_cb(tca8418_keyboard_t *kb, tca8418_key_event_cb_t cb, void *ud);

/* --- WiFi Config UI C types (from wifi_config_ui module) --- */
typedef enum {
    WIFI_CFG_RESULT_NONE,
    WIFI_CFG_RESULT_CONNECTED,
    WIFI_CFG_RESULT_CANCELLED,
} wifi_config_result_t;

typedef struct wifi_config_ui wifi_config_ui_t;
typedef void (*wifi_config_connect_cb_t)(const char *ssid, const char *password, void *ud);

wifi_config_ui_t *wifi_config_ui_create(display_t *display);
void wifi_config_ui_destroy(wifi_config_ui_t *ui);
void wifi_config_ui_set_connect_cb(wifi_config_ui_t *ui, wifi_config_connect_cb_t cb, void *ud);
void wifi_config_ui_start(wifi_config_ui_t *ui);
void wifi_config_ui_start_with_saved(wifi_config_ui_t *ui);
void wifi_config_ui_on_connect_result(wifi_config_ui_t *ui, bool success);
bool wifi_config_ui_is_active(wifi_config_ui_t *ui);
wifi_config_result_t wifi_config_ui_handle_key(wifi_config_ui_t *ui, const tca8418_key_event_t *event);
void wifi_config_ui_update_cursor(wifi_config_ui_t *ui);

#define TAG "CardputerAdv"

#define MIN_BRIGHTNESS 30

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
    tca8418_keyboard_t *keyboard;
    wifi_config_ui_t *wifi_config_ui;
    bool wifi_config_mode;
} cardputer_ctx_t;

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

static int clamp(int val, int lo, int hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static void handle_legacy_key(tca8418_legacy_key_t key, void *ud)
{
    cardputer_ctx_t *ctx = (cardputer_ctx_t *)ud;
    if (ctx->wifi_config_mode) return;

    app_context_t *app = app_get_context();
    audio_codec_t *codec = ctx->codec;
    backlight_t *bl = ctx->backlight;

    switch (key) {
    case TCA8418_KEY_UP: {
        int cur = codec->output_volume;
        int step = (cur <= 20 || cur >= 80) ? 1 : 10;
        int vol = clamp(cur + step, 0, 100);
        if (codec->ops && codec->ops->set_output_volume) codec->ops->set_output_volume(codec, vol);
        char msg[32];
        snprintf(msg, sizeof(msg), "Volume: %d%%", vol);
        if (ctx->display) display_show_notification(ctx->display, msg, 1500);
        break;
    }
    case TCA8418_KEY_DOWN: {
        int cur = codec->output_volume;
        int step = (cur <= 20 || cur >= 80) ? 1 : 10;
        int vol = clamp(cur - step, 0, 100);
        if (codec->ops && codec->ops->set_output_volume) codec->ops->set_output_volume(codec, vol);
        char msg[32];
        snprintf(msg, sizeof(msg), "Volume: %d%%", vol);
        if (ctx->display) display_show_notification(ctx->display, msg, 1500);
        break;
    }
    case TCA8418_KEY_RIGHT: {
        uint8_t cur = backlight_get_brightness(bl);
        int step = (cur <= (MIN_BRIGHTNESS + 20) || cur >= 80) ? 1 : 10;
        int br = clamp((int)cur + step, MIN_BRIGHTNESS, 100);
        backlight_set_brightness(bl, (uint8_t)br, true);
        char msg[32];
        snprintf(msg, sizeof(msg), "Brightness: %d%%", br);
        if (ctx->display) display_show_notification(ctx->display, msg, 1500);
        break;
    }
    case TCA8418_KEY_LEFT: {
        uint8_t cur = backlight_get_brightness(bl);
        int step = (cur <= (MIN_BRIGHTNESS + 20) || cur >= 80) ? 1 : 10;
        int br = clamp((int)cur - step, MIN_BRIGHTNESS, 100);
        backlight_set_brightness(bl, (uint8_t)br, true);
        char msg[32];
        snprintf(msg, sizeof(msg), "Brightness: %d%%", br);
        if (ctx->display) display_show_notification(ctx->display, msg, 1500);
        break;
    }
    case TCA8418_KEY_ENTER:
        if (app && app_get_device_state(app) != kDeviceStateStarting)
            app_toggle_chat(app);
        break;
    default:
        break;
    }
}

static void wifi_connect_cb(const char *ssid, const char *password, void *ud)
{
    cardputer_ctx_t *ctx = (cardputer_ctx_t *)ud;
    ESP_LOGI(TAG, "Attempting WiFi connection to: %s", ssid);

    ssid_manager_add(ssid, password);
    wifi_manager_stop_config_ap();
    wifi_manager_start_station();

    bool connected = false;
    for (int i = 0; i < 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (wifi_manager_is_connected()) {
            connected = true;
            break;
        }
    }

    if (ctx->wifi_config_ui) {
        wifi_config_ui_on_connect_result(ctx->wifi_config_ui, connected);
    }
}

static void handle_key_event(const tca8418_key_event_t *event, void *ud)
{
    cardputer_ctx_t *ctx = (cardputer_ctx_t *)ud;

    if (ctx->wifi_config_mode && ctx->wifi_config_ui) {
        wifi_config_result_t result = wifi_config_ui_handle_key(ctx->wifi_config_ui, event);
        if (result == WIFI_CFG_RESULT_CONNECTED) {
            ESP_LOGI(TAG, "WiFi connected via keyboard config");
            ctx->wifi_config_mode = false;
            wifi_config_ui_destroy(ctx->wifi_config_ui);
            ctx->wifi_config_ui = NULL;
        } else if (result == WIFI_CFG_RESULT_CANCELLED) {
            ESP_LOGI(TAG, "WiFi config cancelled");
            ctx->wifi_config_mode = false;
            wifi_config_ui_destroy(ctx->wifi_config_ui);
            ctx->wifi_config_ui = NULL;
        }
        return;
    }

    app_context_t *app = app_get_context();
    if (app && app_get_device_state(app) == kDeviceStateWifiConfiguring && event->pressed) {
        if (event->key_code == KC_W) {
            ESP_LOGI(TAG, "W key pressed - entering keyboard WiFi config");
            ctx->wifi_config_mode = true;
            ctx->wifi_config_ui = wifi_config_ui_create(ctx->display);
            wifi_config_ui_set_connect_cb(ctx->wifi_config_ui, wifi_connect_cb, ctx);
            wifi_config_ui_start(ctx->wifi_config_ui);
        } else if (event->key_code == KC_S) {
            ESP_LOGI(TAG, "S key pressed - showing saved WiFi list");
            ctx->wifi_config_mode = true;
            ctx->wifi_config_ui = wifi_config_ui_create(ctx->display);
            wifi_config_ui_set_connect_cb(ctx->wifi_config_ui, wifi_connect_cb, ctx);
            wifi_config_ui_start_with_saved(ctx->wifi_config_ui);
        }
    }
}

static void init_i2c(cardputer_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->i2c_bus));
}

static void init_spi(cardputer_ctx_t *ctx)
{
    spi_bus_config_t buscfg = {0};
    buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = DISPLAY_SPI_SCLK_PIN;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_display(cardputer_ctx_t *ctx)
{
    esp_lcd_panel_io_spi_config_t io_config = {0};
    io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
    io_config.dc_gpio_num = DISPLAY_DC_PIN;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.sio_mode = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &ctx->panel_io));

    esp_lcd_panel_dev_config_t panel_cfg = {0};
    panel_cfg.reset_gpio_num = DISPLAY_RST_PIN;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->panel_io, &panel_cfg, &ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(ctx->panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(cardputer_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static void init_keyboard(cardputer_ctx_t *ctx)
{
    ctx->keyboard = tca8418_keyboard_create(ctx->i2c_bus, KEYBOARD_TCA8418_ADDR, KEYBOARD_INT_PIN);
    tca8418_keyboard_initialize(ctx->keyboard);
    tca8418_keyboard_set_key_cb(ctx->keyboard, handle_legacy_key, ctx);
    tca8418_keyboard_set_key_event_cb(ctx->keyboard, handle_key_event, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return "m5stack-cardputer-adv";
}

static void *get_audio_codec(board_desc_t *self)
{
    cardputer_ctx_t *ctx = (cardputer_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
            false, false);
        if (ctx->codec) {
            i2s_channel_disable(ctx->codec->tx_handle);
            i2s_channel_disable(ctx->codec->rx_handle);
        }
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    cardputer_ctx_t *ctx = (cardputer_ctx_t *)self;
    return ctx->display;
}

static void *get_backlight(board_desc_t *self)
{
    cardputer_ctx_t *ctx = (cardputer_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 256);
    }
    return ctx->backlight;
}

static void destroy(board_desc_t *self)
{
    cardputer_ctx_t *ctx = (cardputer_ctx_t *)self;
    if (ctx->backlight) {
        backlight_destroy(ctx->backlight);
        ctx->backlight = NULL;
    }
    board_btn_delete(ctx->boot_button);
    if (ctx->keyboard) tca8418_keyboard_destroy(ctx->keyboard);
    if (ctx->wifi_config_ui) wifi_config_ui_destroy(ctx->wifi_config_ui);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    cardputer_ctx_t *ctx = calloc(1, sizeof(cardputer_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    init_i2c(ctx);
    init_spi(ctx);
    init_display(ctx);
    init_buttons(ctx);
    init_keyboard(ctx);

    backlight_t *bl = (backlight_t *)get_backlight(&ctx->base);
    backlight_restore_brightness(bl);

    return &ctx->base;
}
