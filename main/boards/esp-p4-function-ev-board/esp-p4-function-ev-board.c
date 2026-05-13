#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "backlight.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_vfs_fat.h>

#include "bsp/esp32_p4_function_ev_board.h"
#include "bsp/touch.h"
#include "c_api/board_c_api.h"

#define TAG "ESP32P4FuncEV"

typedef struct {
    board_desc_t base;
    i2c_master_bus_handle_t codec_i2c_bus;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;
    board_btn_t *boot_button;
    esp_lcd_touch_handle_t tp;
} p4_ev_ctx_t;

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

static void init_i2c(p4_ev_ctx_t *ctx)
{
    ESP_ERROR_CHECK(bsp_i2c_init());
    ctx->codec_i2c_bus = bsp_i2c_get_handle();
}

static void init_lcd(p4_ev_ctx_t *ctx)
{
    bsp_display_config_t config = {
        .hdmi_resolution = BSP_HDMI_RES_NONE,
        .dsi_bus = {
            .phy_clk_src = (mipi_dsi_phy_clock_source_t)SOC_MOD_CLK_PLL_F20M,
            .lane_bit_rate_mbps = 1000,
        },
    };

    bsp_lcd_handles_t handles;
    ESP_ERROR_CHECK(bsp_display_new_with_handles(&config, &handles));

    ctx->display = mipi_lcd_display_create(handles.io, handles.panel,
        1024, 600, 0, 0, true, true, false);
}

static void init_buttons(p4_ev_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = 0 };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static void init_touch(p4_ev_ctx_t *ctx)
{
    ESP_ERROR_CHECK(bsp_touch_new(NULL, &ctx->tp));
}

static void init_sd_card(void)
{
    ESP_LOGI(TAG, "Initializing SD card");
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SD card mounted successfully");
    }
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return "esp-p4-function-ev-board";
}

static void *get_audio_codec(board_desc_t *self)
{
    p4_ev_ctx_t *ctx = (p4_ev_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->codec_i2c_bus, (int)BSP_I2C_NUM,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            BSP_I2S_MCLK, BSP_I2S_SCLK, BSP_I2S_LCLK,
            BSP_I2S_DOUT, BSP_I2S_DSIN,
            BSP_POWER_AMP_IO, ES8311_CODEC_DEFAULT_ADDR, true, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    return ((p4_ev_ctx_t *)self)->display;
}

static void *get_backlight(board_desc_t *self)
{
    p4_ev_ctx_t *ctx = (p4_ev_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(BSP_LCD_BACKLIGHT, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void destroy(board_desc_t *self)
{
    p4_ev_ctx_t *ctx = (p4_ev_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    bsp_sdcard_unmount();
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    p4_ev_ctx_t *ctx = calloc(1, sizeof(p4_ev_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    init_i2c(ctx);
    init_lcd(ctx);
    init_buttons(ctx);
    init_touch(ctx);
    init_sd_card();
    backlight_restore_brightness(get_backlight(&ctx->base));

    return &ctx->base;
}
