#include "board_defs.h"
#include "button.h"
#include "system_reset.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "backlight.h"
#include "assets/lang_c.h"
#include "device_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define TAG "LilygoTCameraPlusS3Board"

audio_codec_t *tcamerapluss3_audio_codec_create(int input_sample_rate, int output_sample_rate,
    gpio_num_t mic_bclk, gpio_num_t mic_ws, gpio_num_t mic_data,
    gpio_num_t spkr_bclk, gpio_num_t spkr_lrclk, gpio_num_t spkr_data,
    bool input_reference);

typedef struct {
    uint8_t read_buffer[6];
    int num;
    int x;
    int y;
    i2c_master_dev_handle_t dev_handle;
} cst816x_t;

static uint8_t cst816x_read_reg(cst816x_t *tp, uint8_t reg)
{
    uint8_t val = 0;
    i2c_master_transmit_receive(tp->dev_handle, &reg, 1, &val, 1, pdMS_TO_TICKS(100));
    return val;
}

static void cst816x_read_regs(cst816x_t *tp, uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_master_transmit_receive(tp->dev_handle, &reg, 1, buf, len, pdMS_TO_TICKS(100));
}

static cst816x_t *cst816x_create(i2c_master_bus_handle_t bus, uint8_t addr)
{
    cst816x_t *tp = calloc(1, sizeof(cst816x_t));
    if (!tp) return NULL;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &tp->dev_handle));

    uint8_t chip_id = cst816x_read_reg(tp, 0xA7);
    ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
    return tp;
}

static void cst816x_update(cst816x_t *tp)
{
    cst816x_read_regs(tp, 0x02, tp->read_buffer, 6);
    tp->num = tp->read_buffer[0] & 0x0F;
    tp->x = ((tp->read_buffer[1] & 0x0F) << 8) | tp->read_buffer[2];
    tp->y = ((tp->read_buffer[3] & 0x0F) << 8) | tp->read_buffer[4];
}

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    cst816x_t *touchpad;

    i2c_master_dev_handle_t pmic_dev;

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
    board_btn_t *key1_button;
} tcameraplus_ctx_t;

static uint8_t pmic_read_reg(tcameraplus_ctx_t *ctx, uint8_t reg)
{
    uint8_t val = 0;
    i2c_master_transmit_receive(ctx->pmic_dev, &reg, 1, &val, 1, pdMS_TO_TICKS(100));
    return val;
}

static void pmic_write_reg(tcameraplus_ctx_t *ctx, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    i2c_master_transmit(ctx->pmic_dev, buf, 2, pdMS_TO_TICKS(100));
}

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) {
        return;
    }
    app_toggle_chat(app);
}

static void touchpad_daemon(void *param)
{
    tcameraplus_ctx_t *ctx = (tcameraplus_ctx_t *)param;
    vTaskDelay(pdMS_TO_TICKS(2000));
    bool was_touched = false;
    while (1) {
        cst816x_update(ctx->touchpad);
        if (ctx->touchpad->num > 0) {
            if (!was_touched) {
                was_touched = true;
                app_context_t *app = app_get_context();
                if (app) app_toggle_chat(app);
            }
        } else if (was_touched) {
            was_touched = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void init_i2c(tcameraplus_ctx_t *ctx)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_I2C_SDA_PIN,
        .scl_io_num = TOUCH_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &ctx->i2c_bus));
}

static void init_pmic(tcameraplus_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init Sy6970");
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x6A,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(ctx->i2c_bus, &dev_cfg, &ctx->pmic_dev));

    uint8_t chip_id = pmic_read_reg(ctx, 0x14);
    ESP_LOGI(TAG, "Get sy6970 chip ID: 0x%02X", (chip_id & 0x38));

    pmic_write_reg(ctx, 0x00, 0x08);
    pmic_write_reg(ctx, 0x02, 0xDD);
    pmic_write_reg(ctx, 0x07, 0x8D);
}

static void init_touchpad(tcameraplus_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init CST816x");
    ctx->touchpad = cst816x_create(ctx->i2c_bus, 0x15);
    xTaskCreate(touchpad_daemon, "tp", 2048, ctx, 5, NULL);
}

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_st7789_display(tcameraplus_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS,
        .dc_gpio_num = DISPLAY_DC,
        .spi_mode = 0,
        .pclk_hz = 60 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(tcameraplus_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *board_get_board_type(board_desc_t *self)
{
    (void)self;
    return "LilyGo T-CameraPlus S3";
}

static void *board_get_led(board_desc_t *self)
{
    tcameraplus_ctx_t *ctx = (tcameraplus_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *board_get_audio_codec(board_desc_t *self)
{
    tcameraplus_ctx_t *ctx = (tcameraplus_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = tcamerapluss3_audio_codec_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_MIC_I2S_GPIO_BCLK, AUDIO_MIC_I2S_GPIO_WS, AUDIO_MIC_I2S_GPIO_DATA,
            AUDIO_SPKR_I2S_GPIO_BCLK, AUDIO_SPKR_I2S_GPIO_LRCLK, AUDIO_SPKR_I2S_GPIO_DATA,
            AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *board_get_display(board_desc_t *self)
{
    tcameraplus_ctx_t *ctx = (tcameraplus_ctx_t *)self;
    return ctx->display;
}

static void *board_get_backlight(board_desc_t *self)
{
    tcameraplus_ctx_t *ctx = (tcameraplus_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static bool board_get_battery_level(board_desc_t *self, int *level,
                                    bool *charging, bool *discharging)
{
    tcameraplus_ctx_t *ctx = (tcameraplus_ctx_t *)self;

    uint8_t status = pmic_read_reg(ctx, 0x0B);
    *charging = ((status >> 4) & 0x03) != 0;
    bool is_power_good = (status >> 2) & 0x01;
    *discharging = !(*charging) && is_power_good;

    uint8_t bat_reg = pmic_read_reg(ctx, 0x0E);
    int bat_mv = 2304 + (bat_reg & 0x7F) * 20;
    if (bat_mv >= 4200) *level = 100;
    else if (bat_mv <= 3200) *level = 0;
    else *level = (bat_mv - 3200) * 100 / (4200 - 3200);

    return true;
}

static void board_destroy(board_desc_t *self)
{
    tcameraplus_ctx_t *ctx = (tcameraplus_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->key1_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    tcameraplus_ctx_t *ctx = calloc(1, sizeof(tcameraplus_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = board_get_board_type;
    ctx->base.get_led = board_get_led;
    ctx->base.get_audio_codec = board_get_audio_codec;
    ctx->base.get_display = board_get_display;
    ctx->base.get_backlight = board_get_backlight;
    ctx->base.get_battery_level = board_get_battery_level;
    ctx->base.destroy = board_destroy;

    init_i2c(ctx);
    init_pmic(ctx);
    init_touchpad(ctx);
    init_spi();
    init_st7789_display(ctx);
    init_buttons(ctx);

    backlight_t *bl = (backlight_t *)board_get_backlight(&ctx->base);
    if (bl) backlight_restore_brightness(bl);

    return &ctx->base;
}
