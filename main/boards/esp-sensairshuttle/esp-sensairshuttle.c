#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "assets/lang_c.h"
#include "device_state.h"
#include "i2c_device.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_lcd_ili9341.h"
#include "audio/audio_codec.h"
#include "c_api/board_c_api.h"

#define TAG "ESP_SensairShuttle"

audio_codec_t *adc_pdm_audio_codec_create(
    int input_sample_rate, int output_sample_rate,
    uint32_t adc_mic_channel, int pdm_speak_p, int pdm_speak_n, int pa_ctl);

static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, NULL, 0, 120},
    {0x36, (uint8_t []){0x00}, 1, 0},
    {0x3A, (uint8_t []){0x05}, 1, 0},
    {0xB2, (uint8_t []){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5, 0},
    {0xB7, (uint8_t []){0x05}, 1, 0},
    {0xBB, (uint8_t []){0x21}, 1, 0},
    {0xC0, (uint8_t []){0x2C}, 1, 0},
    {0xC2, (uint8_t []){0x01}, 1, 0},
    {0xC3, (uint8_t []){0x15}, 1, 0},
    {0xC6, (uint8_t []){0x0F}, 1, 0},
    {0xD0, (uint8_t []){0xA7}, 1, 0},
    {0xD0, (uint8_t []){0xA4, 0xA1}, 2, 0},
    {0xD6, (uint8_t []){0xA1}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x05, 0x0E, 0x08, 0x0A, 0x17, 0x39, 0x54,
                         0x4E, 0x37, 0x12, 0x12, 0x31, 0x37}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x10, 0x14, 0x0D, 0x0B, 0x05, 0x39, 0x44,
                         0x4D, 0x38, 0x14, 0x14, 0x2E, 0x35}, 14, 0},
    {0xE4, (uint8_t []){0x23, 0x00, 0x00}, 3, 0},
    {0x21, NULL, 0, 0},
    {0x29, NULL, 0, 0},
    {0x2C, NULL, 0, 0},
};

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    i2c_device_t *touchpad;

    audio_codec_t *codec;
    display_t *display;

    board_btn_t *boot_button;
} sensair_ctx_t;

static void touch_event_task(void *arg)
{
    i2c_device_t *tp = (i2c_device_t *)arg;
    uint8_t buf[6];
    bool was_touched = false;

    while (true) {
        i2c_device_read_regs(tp, 0x02, buf, 6);
        int num = buf[0] & 0x0F;
        bool is_touched = (num > 0);

        if (!is_touched && was_touched) {
            app_context_t *app = app_get_context();
            if (app) {
                if (app_get_device_state(app) == kDeviceStateStarting) {
                    board_enter_wifi_config_mode(board_get_instance());
                } else {
                    app_toggle_chat(app);
                }
            }
        }
        was_touched = is_touched;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

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

static void init_i2c(sensair_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = LCD_TP_SDA,
        .scl_io_num = LCD_TP_SCL,
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

static void init_touchpad(sensair_ctx_t *ctx)
{
    ctx->touchpad = i2c_device_create(ctx->i2c_bus, 0x15);
    xTaskCreate(touch_event_task, "touch_task", 2 * 1024, ctx->touchpad, 5, NULL);
}

static void init_buttons(sensair_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static void init_spi(sensair_ctx_t *ctx)
{
    (void)ctx;
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_CLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * 10 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_lcd_display(sensair_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = DISPLAY_SPI_MODE,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

    const ili9341_vendor_config_t vendor_config = {
        .init_cmds = &vendor_specific_init[0],
        .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RST_PIN,
        .rgb_ele_order = DISPLAY_RGB_ORDER,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_set_gap(panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static const char *ss_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *ss_get_audio_codec(board_desc_t *self)
{
    sensair_ctx_t *ctx = (sensair_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = adc_pdm_audio_codec_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_ADC_MIC_CHANNEL,
            AUDIO_PDM_SPEAK_P_GPIO, AUDIO_PDM_SPEAK_N_GPIO,
            AUDIO_PA_CTL_GPIO);
    }
    return ctx->codec;
}

static void *ss_get_display(board_desc_t *self)
{
    sensair_ctx_t *ctx = (sensair_ctx_t *)self;
    return ctx->display;
}

static void ss_destroy(board_desc_t *self)
{
    sensair_ctx_t *ctx = (sensair_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    if (ctx->touchpad) i2c_device_destroy(ctx->touchpad);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    sensair_ctx_t *ctx = calloc(1, sizeof(sensair_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = ss_get_board_type;
    ctx->base.get_audio_codec = ss_get_audio_codec;
    ctx->base.get_display = ss_get_display;
    ctx->base.destroy = ss_destroy;

    init_i2c(ctx);
    init_touchpad(ctx);
    init_buttons(ctx);
    init_spi(ctx);
    init_lcd_display(ctx);

    return &ctx->base;
}
