#include "board_defs.h"
#include "audio_codec.h"
#include "c_api/display_c_api.h"
#include "c_api/app_c_api.h"
#include "backlight.h"
#include "power_save_timer.h"
#include "i2c_device.h"
#include "axp2101.h"
#include "config.h"
#include "device_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include <esp_timer.h>

audio_codec_t *cores3_audio_codec_create(void *i2c_master_handle,
    int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    uint8_t aw88298_addr, uint8_t es7210_addr, bool input_reference);

#define TAG "M5StackCoreS3Board"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    axp2101_t *pmic;
    i2c_device_t *aw9523;
    i2c_device_t *ft6336;
    uint8_t *ft6336_buf;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;
    void *camera;
    power_save_timer_t *pst;
    esp_timer_handle_t touchpad_timer;

    int touch_num;
    int touch_x;
    int touch_y;
} cores3_ctx_t;

static void pmic_init(cores3_ctx_t *ctx)
{
    ctx->pmic = axp2101_create(ctx->i2c_bus, 0x34);
    i2c_device_t *dev = axp2101_get_i2c_device(ctx->pmic);
    uint8_t data = i2c_device_read_reg(dev, 0x90);
    data |= 0b10110100;
    i2c_device_write_reg(dev, 0x90, data);
    i2c_device_write_reg(dev, 0x99, (0b11110 - 5));
    i2c_device_write_reg(dev, 0x97, (0b11110 - 2));
    i2c_device_write_reg(dev, 0x69, 0b00110101);
    i2c_device_write_reg(dev, 0x30, 0b111111);
    i2c_device_write_reg(dev, 0x90, 0xBF);
    i2c_device_write_reg(dev, 0x94, 33 - 5);
    i2c_device_write_reg(dev, 0x95, 33 - 5);
}

static void pmic_set_brightness(cores3_ctx_t *ctx, uint8_t brightness)
{
    brightness = ((brightness + 641) >> 5);
    i2c_device_write_reg(axp2101_get_i2c_device(ctx->pmic), 0x99, brightness);
}

static void custom_bl_set(void *impl_ctx, uint8_t brightness)
{
    pmic_set_brightness((cores3_ctx_t *)impl_ctx, brightness);
}

static void aw9523_init(cores3_ctx_t *ctx)
{
    ctx->aw9523 = i2c_device_create(ctx->i2c_bus, 0x58);
    i2c_device_write_reg(ctx->aw9523, 0x02, 0b00000111);
    i2c_device_write_reg(ctx->aw9523, 0x03, 0b10001111);
    i2c_device_write_reg(ctx->aw9523, 0x04, 0b00011000);
    i2c_device_write_reg(ctx->aw9523, 0x05, 0b00001100);
    i2c_device_write_reg(ctx->aw9523, 0x11, 0b00010000);
    i2c_device_write_reg(ctx->aw9523, 0x12, 0b11111111);
    i2c_device_write_reg(ctx->aw9523, 0x13, 0b11111111);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void aw9523_reset_ili9342(cores3_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Reset IlI9342");
    i2c_device_write_reg(ctx->aw9523, 0x03, 0b10000001);
    vTaskDelay(pdMS_TO_TICKS(20));
    i2c_device_write_reg(ctx->aw9523, 0x03, 0b10000011);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void ft6336_init(cores3_ctx_t *ctx)
{
    ctx->ft6336 = i2c_device_create(ctx->i2c_bus, 0x38);
    uint8_t chip_id = i2c_device_read_reg(ctx->ft6336, 0xA3);
    ESP_LOGI(TAG, "FT6336 chip ID: 0x%02X", chip_id);
    ctx->ft6336_buf = calloc(6, 1);
}

static void poll_touchpad_cb(void *arg)
{
    cores3_ctx_t *ctx = (cores3_ctx_t *)arg;
    static bool was_touched = false;
    static int64_t touch_start_time = 0;
    const int64_t TOUCH_THRESHOLD_MS = 500;

    i2c_device_read_regs(ctx->ft6336, 0x02, ctx->ft6336_buf, 6);
    ctx->touch_num = ctx->ft6336_buf[0] & 0x0F;
    ctx->touch_x = ((ctx->ft6336_buf[1] & 0x0F) << 8) | ctx->ft6336_buf[2];
    ctx->touch_y = ((ctx->ft6336_buf[3] & 0x0F) << 8) | ctx->ft6336_buf[4];

    if (ctx->touch_num > 0 && !was_touched) {
        was_touched = true;
        touch_start_time = esp_timer_get_time() / 1000;
    } else if (ctx->touch_num == 0 && was_touched) {
        was_touched = false;
        int64_t duration = (esp_timer_get_time() / 1000) - touch_start_time;
        if (duration < TOUCH_THRESHOLD_MS) {
            app_context_t *app = app_get_context();
            if (app) {
                if (app_get_device_state(app) == kDeviceStateStarting) return;
                app_toggle_chat(app);
            }
        }
    }
}

static void init_i2c(cores3_ctx_t *ctx)
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

static void init_spi(cores3_ctx_t *ctx)
{
    spi_bus_config_t buscfg = {0};
    buscfg.mosi_io_num = GPIO_NUM_37;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = GPIO_NUM_36;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_display(cores3_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {0};
    io_config.cs_gpio_num = GPIO_NUM_3;
    io_config.dc_gpio_num = GPIO_NUM_35;
    io_config.spi_mode = 2;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_cfg = {0};
    panel_cfg.reset_gpio_num = GPIO_NUM_NC;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_cfg, &panel));

    esp_lcd_panel_reset(panel);
    aw9523_reset_ili9342(ctx);

    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_camera(cores3_ctx_t *ctx)
{
    (void)ctx;
    /* Camera initialization requires EspVideo C++ class.
       Omitted: the C board wrapper's get_camera returns NULL by default. */
}

static void init_touchpad(cores3_ctx_t *ctx)
{
    ft6336_init(ctx);
    esp_timer_create_args_t timer_args = {
        .callback = poll_touchpad_cb,
        .arg = ctx,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "touchpad_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &ctx->touchpad_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(ctx->touchpad_timer, 20 * 1000));
}

static void on_pst_enter_sleep(void *ud)
{
    cores3_ctx_t *ctx = (cores3_ctx_t *)ud;
    if (ctx->display) display_set_power_save_mode(ctx->display, true);
    if (ctx->backlight) backlight_set_brightness(ctx->backlight, 10, false);
}

static void on_pst_exit_sleep(void *ud)
{
    cores3_ctx_t *ctx = (cores3_ctx_t *)ud;
    if (ctx->display) display_set_power_save_mode(ctx->display, false);
    if (ctx->backlight) backlight_restore_brightness(ctx->backlight);
}

static void on_pst_shutdown(void *ud)
{
    cores3_ctx_t *ctx = (cores3_ctx_t *)ud;
    axp2101_power_off(ctx->pmic);
}

static void init_power_save(cores3_ctx_t *ctx)
{
    power_save_timer_cfg_t cfg = { .cpu_max_freq = -1, .seconds_to_sleep = 60, .seconds_to_shutdown = 300 };
    ctx->pst = power_save_timer_create(&cfg);
    power_save_timer_on_enter_sleep(ctx->pst, on_pst_enter_sleep, ctx);
    power_save_timer_on_exit_sleep(ctx->pst, on_pst_exit_sleep, ctx);
    power_save_timer_on_shutdown(ctx->pst, on_pst_shutdown, ctx);
    power_save_timer_set_enabled(ctx->pst, true);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return "m5stack-core-s3";
}

static void *get_audio_codec(board_desc_t *self)
{
    cores3_ctx_t *ctx = (cores3_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = cores3_audio_codec_create(ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_AW88298_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    return ((cores3_ctx_t *)self)->display;
}

static void *get_backlight(board_desc_t *self)
{
    cores3_ctx_t *ctx = (cores3_ctx_t *)self;
    if (!ctx->backlight) {
        backlight_impl_t impl = {
            .set_brightness = custom_bl_set,
            .destroy = NULL,
            .impl_ctx = ctx,
        };
        ctx->backlight = backlight_create(&impl);
    }
    return ctx->backlight;
}

static bool get_battery_level(board_desc_t *self, int *level, bool *charging, bool *discharging)
{
    cores3_ctx_t *ctx = (cores3_ctx_t *)self;
    static bool last_discharging = false;
    *charging = axp2101_is_charging(ctx->pmic);
    *discharging = axp2101_is_discharging(ctx->pmic);
    if (*discharging != last_discharging) {
        power_save_timer_set_enabled(ctx->pst, *discharging);
        last_discharging = *discharging;
    }
    *level = axp2101_get_battery_level(ctx->pmic);
    return true;
}

static void destroy(board_desc_t *self)
{
    cores3_ctx_t *ctx = (cores3_ctx_t *)self;
    if (ctx->touchpad_timer) esp_timer_stop(ctx->touchpad_timer);
    free(ctx->ft6336_buf);
    if (ctx->ft6336) i2c_device_destroy(ctx->ft6336);
    if (ctx->aw9523) i2c_device_destroy(ctx->aw9523);
    if (ctx->pst) power_save_timer_destroy(ctx->pst);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    cores3_ctx_t *ctx = calloc(1, sizeof(cores3_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.get_battery_level = get_battery_level;
    ctx->base.destroy = destroy;

    init_power_save(ctx);
    init_i2c(ctx);
    pmic_init(ctx);
    aw9523_init(ctx);
    init_spi(ctx);
    init_display(ctx);
    init_camera(ctx);
    init_touchpad(ctx);

    backlight_t *bl = (backlight_t *)get_backlight(&ctx->base);
    backlight_restore_brightness(bl);

    return &ctx->base;
}
