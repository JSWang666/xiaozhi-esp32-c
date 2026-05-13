#include "board_defs.h"
#include "audio_codec.h"
#include "c_api/display_c_api.h"
#include "c_api/app_c_api.h"
#include "backlight.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "device_state.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_st7123.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_check.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <esp_lcd_touch_gt911.h>
#include <esp_lcd_touch_st7123.h>

audio_codec_t *tab5_audio_codec_create(void *i2c_master_handle,
    int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8388_addr, uint8_t es7210_addr, bool input_reference);

#define TAG "M5StackTab5Board"

#define AUDIO_CODEC_ES8388_ADDR ES8388_CODEC_DEFAULT_ADDR
#define LCD_MIPI_DSI_PHY_PWR_LDO_CHAN       3
#define LCD_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define ST7123_TOUCH_I2C_ADDRESS            0x55

#define PI4IO_REG_CHIP_RESET 0x01
#define PI4IO_REG_IO_DIR     0x03
#define PI4IO_REG_OUT_SET    0x05
#define PI4IO_REG_OUT_H_IM   0x07
#define PI4IO_REG_IN_DEF_STA 0x09
#define PI4IO_REG_PULL_EN    0x0B
#define PI4IO_REG_PULL_SEL   0x0D
#define PI4IO_REG_IN_STA     0x0F
#define PI4IO_REG_INT_MASK   0x11
#define PI4IO_REG_IRQ_STA    0x13

#define setbit(x, bit)  ((x) |= (1U << (bit)))
#define clrbit(x, bit)  ((x) &= ~(1U << (bit)))

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    i2c_device_t *pi4ioe1;
    i2c_device_t *pi4ioe2;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
    esp_lcd_touch_handle_t touch;
} tab5_ctx_t;

static void pi4ioe1_init(tab5_ctx_t *ctx)
{
    ctx->pi4ioe1 = i2c_device_create(ctx->i2c_bus, 0x43);
    i2c_device_write_reg(ctx->pi4ioe1, PI4IO_REG_CHIP_RESET, 0xFF);
    (void)i2c_device_read_reg(ctx->pi4ioe1, PI4IO_REG_CHIP_RESET);
    i2c_device_write_reg(ctx->pi4ioe1, PI4IO_REG_IO_DIR, 0b01111111);
    i2c_device_write_reg(ctx->pi4ioe1, PI4IO_REG_OUT_H_IM, 0b00000000);
    i2c_device_write_reg(ctx->pi4ioe1, PI4IO_REG_PULL_SEL, 0b01111111);
    i2c_device_write_reg(ctx->pi4ioe1, PI4IO_REG_PULL_EN, 0b01111111);
    i2c_device_write_reg(ctx->pi4ioe1, PI4IO_REG_IN_DEF_STA, 0b10000000);
    i2c_device_write_reg(ctx->pi4ioe1, PI4IO_REG_INT_MASK, 0b01111111);
    i2c_device_write_reg(ctx->pi4ioe1, PI4IO_REG_OUT_SET, 0b01110110);
}

static void pi4ioe2_init(tab5_ctx_t *ctx)
{
    ctx->pi4ioe2 = i2c_device_create(ctx->i2c_bus, 0x44);
    i2c_device_write_reg(ctx->pi4ioe2, PI4IO_REG_CHIP_RESET, 0xFF);
    (void)i2c_device_read_reg(ctx->pi4ioe2, PI4IO_REG_CHIP_RESET);
    i2c_device_write_reg(ctx->pi4ioe2, PI4IO_REG_IO_DIR, 0b10111001);
    i2c_device_write_reg(ctx->pi4ioe2, PI4IO_REG_OUT_H_IM, 0b00000110);
    i2c_device_write_reg(ctx->pi4ioe2, PI4IO_REG_PULL_SEL, 0b10111001);
    i2c_device_write_reg(ctx->pi4ioe2, PI4IO_REG_PULL_EN, 0b11111001);
    i2c_device_write_reg(ctx->pi4ioe2, PI4IO_REG_IN_DEF_STA, 0b01000000);
    i2c_device_write_reg(ctx->pi4ioe2, PI4IO_REG_INT_MASK, 0b10111111);
    i2c_device_write_reg(ctx->pi4ioe2, PI4IO_REG_OUT_SET, 0b10001001);
}

static void pi4ioe_set_bit(i2c_device_t *dev, int bit, bool set)
{
    uint8_t val = i2c_device_read_reg(dev, PI4IO_REG_OUT_SET);
    if (set) setbit(val, bit); else clrbit(val, bit);
    i2c_device_write_reg(dev, PI4IO_REG_OUT_SET, val);
}

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void init_i2c(tab5_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = 1,
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

static void init_ili9881c_display(tab5_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = LCD_MIPI_DSI_PHY_PWR_LDO_CHAN, .voltage_mv = LCD_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_dsi_bus_config_t bus_cfg = { .bus_id = 0, .num_data_lanes = 2, .lane_bit_rate_mbps = 900 };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    esp_lcd_dbi_io_config_t dbi_cfg = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &panel_io));

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 60,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = DISPLAY_WIDTH, .v_size = DISPLAY_HEIGHT,
            .hsync_pulse_width = 40, .hsync_back_porch = 140, .hsync_front_porch = 40,
            .vsync_pulse_width = 4, .vsync_back_porch = 20, .vsync_front_porch = 20,
        },
    };

    ili9881c_vendor_config_t vendor_cfg = {
        .init_cmds = tab5_lcd_ili9881c_specific_init_code_default,
        .init_cmds_size = sizeof(tab5_lcd_ili9881c_specific_init_code_default) / sizeof(tab5_lcd_ili9881c_specific_init_code_default[0]),
        .mipi_config = { .dsi_bus = dsi_bus, .dpi_config = &dpi_cfg, .lane_num = 2 },
    };

    esp_lcd_panel_dev_config_t dev_cfg = {0};
    dev_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    dev_cfg.reset_gpio_num = -1;
    dev_cfg.bits_per_pixel = 16;
    dev_cfg.vendor_config = &vendor_cfg;

    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9881c(panel_io, &dev_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ctx->display = mipi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_gt911_touch(tab5_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init GT911");
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_WIDTH, .y_max = DISPLAY_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = TOUCH_INT_GPIO,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
        .control_phase_bytes = 1, .dc_bit_offset = 0, .lcd_cmd_bits = 16,
        .scl_speed_hz = 100000,
        .flags = { .disable_control_phase = 1 },
    };
    esp_lcd_new_panel_io_i2c(ctx->i2c_bus, &tp_io_cfg, &tp_io);
    esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &ctx->touch);
}

static void init_st7123_display(tab5_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;

    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = LCD_MIPI_DSI_PHY_PWR_LDO_CHAN, .voltage_mv = LCD_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    esp_lcd_dsi_bus_config_t bus_cfg = { .bus_id = 0, .num_data_lanes = 2, .lane_bit_rate_mbps = 965 };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    esp_lcd_dbi_io_config_t dbi_cfg = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io));

    esp_lcd_dpi_panel_config_t dpi_cfg = {0};
    dpi_cfg.virtual_channel = 0;
    dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_cfg.dpi_clock_freq_mhz = 70;
    dpi_cfg.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dpi_cfg.num_fbs = 1;
    dpi_cfg.video_timing.h_size = 720;
    dpi_cfg.video_timing.v_size = 1280;
    dpi_cfg.video_timing.hsync_pulse_width = 2;
    dpi_cfg.video_timing.hsync_back_porch = 40;
    dpi_cfg.video_timing.hsync_front_porch = 40;
    dpi_cfg.video_timing.vsync_pulse_width = 2;
    dpi_cfg.video_timing.vsync_back_porch = 8;
    dpi_cfg.video_timing.vsync_front_porch = 220;
    dpi_cfg.flags.use_dma2d = true;

    st7123_vendor_config_t vendor_cfg = {0};
    vendor_cfg.init_cmds = st7123_vendor_specific_init_default;
    vendor_cfg.init_cmds_size = sizeof(st7123_vendor_specific_init_default) / sizeof(st7123_vendor_specific_init_default[0]);
    vendor_cfg.mipi_config.dsi_bus = dsi_bus;
    vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
    vendor_cfg.mipi_config.lane_num = 2;

    esp_lcd_panel_dev_config_t dev_cfg = {0};
    dev_cfg.reset_gpio_num = -1;
    dev_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    dev_cfg.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    dev_cfg.bits_per_pixel = 24;
    dev_cfg.vendor_config = &vendor_cfg;

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7123(io, &dev_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ctx->display = mipi_lcd_display_create(io, panel,
        720, 1280, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_st7123_touch(tab5_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init ST7123 Touch");
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = 720, .y_max = 1280,
        .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = TOUCH_INT_GPIO,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr = 0x55,
        .control_phase_bytes = 1, .dc_bit_offset = 0,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(ctx->i2c_bus, &tp_io_cfg, &tp_io));
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_st7123(tp_io, &tp_cfg, &ctx->touch));
}

static void init_display_auto(tab5_ctx_t *ctx)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t ret = i2c_master_probe(ctx->i2c_bus, ST7123_TOUCH_I2C_ADDRESS, 200);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Detected ST7123 at 0x%02X", ST7123_TOUCH_I2C_ADDRESS);
        init_st7123_display(ctx);
        init_st7123_touch(ctx);
    } else {
        ESP_LOGI(TAG, "ST7123 not found, using default ST7703+GT911");
        init_ili9881c_display(ctx);
        init_gt911_touch(ctx);
    }
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return "m5stack-tab5";
}

static void *get_audio_codec(board_desc_t *self)
{
    tab5_ctx_t *ctx = (tab5_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = tab5_audio_codec_create(ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8388_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    return ((tab5_ctx_t *)self)->display;
}

static void *get_backlight(board_desc_t *self)
{
    tab5_ctx_t *ctx = (tab5_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void destroy(board_desc_t *self)
{
    tab5_ctx_t *ctx = (tab5_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    if (ctx->pi4ioe1) i2c_device_destroy(ctx->pi4ioe1);
    if (ctx->pi4ioe2) i2c_device_destroy(ctx->pi4ioe2);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    tab5_ctx_t *ctx = calloc(1, sizeof(tab5_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    init_i2c(ctx);
    ESP_LOGI(TAG, "Init I/O Expander PI4IOE");
    pi4ioe1_init(ctx);
    pi4ioe2_init(ctx);
    init_display_auto(ctx);

    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    /* Power control: charge QC, charge, USB 5V, ext 5V */
    if (ctx->pi4ioe2) pi4ioe_set_bit(ctx->pi4ioe2, 5, false);  /* CHG_QC_EN active low */
    if (ctx->pi4ioe2) pi4ioe_set_bit(ctx->pi4ioe2, 7, true);   /* CHG_EN */
    if (ctx->pi4ioe2) pi4ioe_set_bit(ctx->pi4ioe2, 3, true);   /* USB5V_EN */
    if (ctx->pi4ioe1) pi4ioe_set_bit(ctx->pi4ioe1, 2, true);   /* EXT5V_EN */

    backlight_t *bl = (backlight_t *)get_backlight(&ctx->base);
    backlight_restore_brightness(bl);

    return &ctx->base;
}
