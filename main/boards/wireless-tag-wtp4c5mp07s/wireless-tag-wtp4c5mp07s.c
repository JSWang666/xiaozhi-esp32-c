#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "backlight.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_ek79007.h>
#include <driver/gpio.h>

#define TAG "WirelessTagEsp32p47b"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
} wt_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void init_codec_i2c(wt_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_1,
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
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->i2c_bus));
}

static void init_lcd(wt_ctx_t *ctx)
{
#if MIPI_DSI_PHY_PWR_LDO_CHAN > 0
    static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t disp_panel = NULL;

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .lane_bit_rate_mbps = 900,
    };
    esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);

    esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
    esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io);

    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 52,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = 1024,
            .v_size = 600,
            .hsync_pulse_width = 10,
            .hsync_back_porch = 160,
            .hsync_front_porch = 160,
            .vsync_pulse_width = 1,
            .vsync_back_porch = 23,
            .vsync_front_porch = 12,
        },
        .flags = {
            .use_dma2d = true,
        },
    };
    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };

    const esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .flags = {
            .reset_active_high = true,
        },
        .vendor_config = &vendor_config,
    };
    esp_lcd_new_panel_ek79007(io, &lcd_dev_config, &disp_panel);
    esp_lcd_panel_reset(disp_panel);
    esp_lcd_panel_init(disp_panel);

    ctx->display = mipi_lcd_display_create(io, disp_panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(wt_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *get_board_type(board_desc_t *self) { (void)self; return BOARD_TYPE; }

static void *get_audio_codec(board_desc_t *self)
{
    wt_ctx_t *ctx = (wt_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->i2c_bus, I2C_NUM_1,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, false, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self) { return ((wt_ctx_t *)self)->display; }

static void *get_backlight(board_desc_t *self)
{
    wt_ctx_t *ctx = (wt_ctx_t *)self;
    if (!ctx->backlight)
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    return ctx->backlight;
}

static void board_destroy(board_desc_t *self)
{
    wt_ctx_t *ctx = (wt_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    wt_ctx_t *ctx = calloc(1, sizeof(wt_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = board_destroy;

    init_codec_i2c(ctx);
    init_lcd(ctx);
    init_buttons(ctx);

    backlight_t *bl = get_backlight(&ctx->base);
    if (bl && bl->ops && bl->ops->restore_brightness)
        bl->ops->restore_brightness(bl);

    return &ctx->base;
}
