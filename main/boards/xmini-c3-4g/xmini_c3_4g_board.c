#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_i2c.h>

#define TAG "XminiC3Board"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t codec_i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;

    board_btn_t *boot_button;
} xmini_c3_4g_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    app_toggle_chat(app);
}

static void init_codec_i2c(xmini_c3_4g_ctx_t *ctx)
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

static void init_ssd1306_display(xmini_c3_4g_ctx_t *ctx)
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
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(ctx->codec_i2c_bus, &io_config, &ctx->panel_io));

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

static void init_buttons(xmini_c3_4g_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_led(board_desc_t *self)
{
    xmini_c3_4g_ctx_t *ctx = (xmini_c3_4g_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    xmini_c3_4g_ctx_t *ctx = (xmini_c3_4g_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->codec_i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    xmini_c3_4g_ctx_t *ctx = (xmini_c3_4g_ctx_t *)self;
    return ctx->display;
}

static void destroy(board_desc_t *self)
{
    xmini_c3_4g_ctx_t *ctx = (xmini_c3_4g_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    xmini_c3_4g_ctx_t *ctx = calloc(1, sizeof(xmini_c3_4g_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_ML307;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.destroy = destroy;

    init_codec_i2c(ctx);
    init_ssd1306_display(ctx);
    init_buttons(ctx);

    return &ctx->base;
}
