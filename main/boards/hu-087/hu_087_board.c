#include "board_defs.h"
#include "button.h"
#include "system_reset.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "audio/codecs/no_audio_codec.h"
#include "device_state.h"

#include <stdlib.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_i2c.h>
#include "c_api/board_c_api.h"

#define TAG "Hu087Board"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t display_i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;

    board_btn_t *touch_button;
} hu087_ctx_t;

static void on_touch_click(void *ud)
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

static void init_display_i2c(hu087_ctx_t *ctx)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = 0,
        .sda_io_num = DISPLAY_SDA_PIN,
        .scl_io_num = DISPLAY_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &ctx->display_i2c_bus));
}

static void init_ssd1306_display(hu087_ctx_t *ctx)
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

static void init_amp_ctrl(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << AUDIO_I2S_SPK_GPIO_CTLR),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(AUDIO_I2S_SPK_GPIO_CTLR, 1);
}

static void init_buttons(hu087_ctx_t *ctx)
{
    board_btn_gpio_cfg_t touch_cfg = { .gpio_num = TOUCH_BUTTON_GPIO };
    ctx->touch_button = board_btn_create_gpio(&touch_cfg);
    board_btn_on_click(ctx->touch_button, on_touch_click, ctx);
}

static const char *hu087_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *hu087_get_audio_codec(board_desc_t *self)
{
    hu087_ctx_t *ctx = (hu087_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = no_audio_codec_simplex_create_ex(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            I2S_STD_SLOT_RIGHT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN,
            I2S_STD_SLOT_RIGHT);
    }
    return ctx->codec;
}

static void *hu087_get_display(board_desc_t *self)
{
    hu087_ctx_t *ctx = (hu087_ctx_t *)self;
    return ctx->display;
}

static void hu087_destroy(board_desc_t *self)
{
    hu087_ctx_t *ctx = (hu087_ctx_t *)self;
    board_btn_delete(ctx->touch_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    hu087_ctx_t *ctx = calloc(1, sizeof(hu087_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = hu087_get_board_type;
    ctx->base.get_audio_codec = hu087_get_audio_codec;
    ctx->base.get_display = hu087_get_display;
    ctx->base.destroy = hu087_destroy;

    init_display_i2c(ctx);
    init_ssd1306_display(ctx);
    init_buttons(ctx);
    init_amp_ctrl();

    return &ctx->base;
}
