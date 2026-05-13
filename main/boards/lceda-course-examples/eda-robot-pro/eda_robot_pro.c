/*
 * EDA Robot Pro board descriptor (C port)
 */
#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "audio/codecs/no_audio_codec.h"

#include <stdlib.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define TAG "EDARobotPro"

extern void InitializeEDARobotDogController(void);

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t display_i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    display_t *display;
    audio_codec_t *codec;

    board_btn_t *boot_button;
    board_btn_t *touch_button;
} eda_robot_pro_ctx_t;

/* ── button callbacks ── */

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

/* ── hardware init ── */

static void init_display_i2c(eda_robot_pro_ctx_t *ctx)
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

static void init_ssd1306_display(eda_robot_pro_ctx_t *ctx)
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
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(ctx->panel, false));

    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(ctx->panel, true));

    ctx->display = oled_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
}

static void init_buttons(eda_robot_pro_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = {.gpio_num = BOOT_BUTTON_GPIO};
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);

    board_btn_gpio_cfg_t touch_cfg = {.gpio_num = TOUCH_BUTTON_GPIO};
    ctx->touch_button = board_btn_create_gpio(&touch_cfg);
    board_btn_on_press_down(ctx->touch_button, on_touch_press_down, ctx);
    board_btn_on_press_up(ctx->touch_button, on_touch_press_up, ctx);
}

/* ── vtable implementations ── */

static const char *erp_get_board_type(board_desc_t *self)
{
    (void)self;
    return "EDARobotPro";
}

static void *erp_get_audio_codec(board_desc_t *self)
{
    eda_robot_pro_ctx_t *ctx = (eda_robot_pro_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = no_audio_codec_simplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
    }
    return ctx->codec;
}

static void *erp_get_display(board_desc_t *self)
{
    eda_robot_pro_ctx_t *ctx = (eda_robot_pro_ctx_t *)self;
    return ctx->display;
}

static void erp_destroy(board_desc_t *self)
{
    eda_robot_pro_ctx_t *ctx = (eda_robot_pro_ctx_t *)self;
    if (ctx->boot_button)  board_btn_delete(ctx->boot_button);
    if (ctx->touch_button) board_btn_delete(ctx->touch_button);
    free(ctx);
}

/* ── board entry point ── */

board_desc_t *create_board_desc(void)
{
    eda_robot_pro_ctx_t *ctx = calloc(1, sizeof(eda_robot_pro_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind             = BOARD_KIND_WIFI;
    ctx->base.get_board_type   = erp_get_board_type;
    ctx->base.get_audio_codec  = erp_get_audio_codec;
    ctx->base.get_display      = erp_get_display;
    ctx->base.destroy          = erp_destroy;

    init_display_i2c(ctx);
    init_ssd1306_display(ctx);
    InitializeEDARobotDogController();
    init_buttons(ctx);

    return &ctx->base;
}
