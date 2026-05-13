#include "board_defs.h"
#include "button.h"
#include "system_reset.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
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
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>

#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "hi8561_driver.h"
#include "rm69a10_driver.h"
#endif

#define TAG "LilygoTDisplayP4Board"

/* Pin definitions from t_display_p4_config.h (raw values, C-compatible) */
#define P4_IIC_1_SDA            7
#define P4_IIC_1_SCL            8
#define P4_IIC_2_SDA            20
#define P4_IIC_2_SCL            21

#define P4_XL9535_ADDR          0x20
#define P4_ES8311_ADDR          0x18

#define P4_ES8311_SDA           P4_IIC_2_SDA
#define P4_ES8311_SCL           P4_IIC_2_SCL
#define P4_ES8311_ADC_DATA      11
#define P4_ES8311_DAC_DATA      10
#define P4_ES8311_BCLK          12
#define P4_ES8311_MCLK          13
#define P4_ES8311_WS_LRCK       9

#define P4_BOOT_BUTTON_GPIO     GPIO_NUM_35
#define P4_HI8561_SCREEN_BL     51

#define P4_AUDIO_INPUT_SAMPLE_RATE  24000
#define P4_AUDIO_OUTPUT_SAMPLE_RATE 24000

/* XL9535 pin encoding: port = pin / 10, bit = pin % 10 */
#define XL9535_PIN_IO0   0
#define XL9535_PIN_IO1   1
#define XL9535_PIN_IO2   2
#define XL9535_PIN_IO3   3
#define XL9535_PIN_IO4   4
#define XL9535_PIN_IO5   5
#define XL9535_PIN_IO6   6
#define XL9535_PIN_IO7   7
#define XL9535_PIN_IO10  10
#define XL9535_PIN_IO11  11
#define XL9535_PIN_IO12  12
#define XL9535_PIN_IO13  13
#define XL9535_PIN_IO14  14
#define XL9535_PIN_IO15  15
#define XL9535_PIN_IO16  16
#define XL9535_PIN_IO17  17

#define XL_3_3_V_POWER_EN       XL9535_PIN_IO0
#define XL_SCREEN_RST           XL9535_PIN_IO2
#define XL_TOUCH_RST            XL9535_PIN_IO3
#define XL_5_0_V_POWER_EN       XL9535_PIN_IO6
#define XL_ESP32P4_VCCA_POWER_EN XL9535_PIN_IO10
#define XL_GPS_WAKE_UP          XL9535_PIN_IO11
#define XL_ESP32C6_EN           XL9535_PIN_IO14

#if defined CONFIG_SCREEN_PIXEL_FORMAT_RGB565
#define P4_SCREEN_BITS_PER_PIXEL 16
#define P4_SCREEN_PIXEL_FMT LCD_COLOR_PIXEL_FORMAT_RGB565
#elif defined CONFIG_SCREEN_PIXEL_FORMAT_RGB888
#define P4_SCREEN_BITS_PER_PIXEL 24
#define P4_SCREEN_PIXEL_FMT LCD_COLOR_PIXEL_FORMAT_RGB888
#endif

#if defined CONFIG_SCREEN_TYPE_HI8561
#define P4_SCREEN_WIDTH   540
#define P4_SCREEN_HEIGHT  1168
#define P4_DSI_DPI_CLK    60
#define P4_DSI_HSYNC      28
#define P4_DSI_HBP        26
#define P4_DSI_HFP        20
#define P4_DSI_VSYNC      2
#define P4_DSI_VBP        22
#define P4_DSI_VFP        200
#define P4_DATA_LANES     2
#define P4_LANE_RATE      1000
#define P4_TOUCH_ADDR     0x68
#elif defined CONFIG_SCREEN_TYPE_RM69A10
#define P4_SCREEN_WIDTH   568
#define P4_SCREEN_HEIGHT  1232
#define P4_DSI_DPI_CLK    60
#define P4_DSI_HSYNC      50
#define P4_DSI_HBP        150
#define P4_DSI_HFP        50
#define P4_DSI_VSYNC      40
#define P4_DSI_VBP        120
#define P4_DSI_VFP        80
#define P4_DATA_LANES     2
#define P4_LANE_RATE      1000
#define P4_TOUCH_ADDR     0x5D
#endif

/* ---------- XL9535 I2C GPIO expander driver ---------- */

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t output[2];
    uint8_t config[2];
} xl9535_t;

static esp_err_t xl9535_write_reg(xl9535_t *x, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(x->dev, buf, 2, pdMS_TO_TICKS(100));
}

static xl9535_t *xl9535_create(i2c_master_bus_handle_t bus, uint8_t addr)
{
    xl9535_t *x = calloc(1, sizeof(xl9535_t));
    if (!x) return NULL;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &x->dev));
    x->config[0] = 0xFF;
    x->config[1] = 0xFF;
    x->output[0] = 0x00;
    x->output[1] = 0x00;
    return x;
}

static void xl9535_pin_mode_output(xl9535_t *x, int pin)
{
    int port = pin / 10;
    int bit = pin % 10;
    x->config[port] &= ~(1 << bit);
    xl9535_write_reg(x, 0x06 + port, x->config[port]);
}

static void xl9535_pin_write(xl9535_t *x, int pin, bool high)
{
    int port = pin / 10;
    int bit = pin % 10;
    if (high)
        x->output[port] |= (1 << bit);
    else
        x->output[port] &= ~(1 << bit);
    xl9535_write_reg(x, 0x02 + port, x->output[port]);
}

/* ---------- Touch I2C helper ---------- */

typedef struct {
    i2c_master_dev_handle_t dev;
    int finger_count;
} touch_dev_t;

static touch_dev_t *touch_dev_create(i2c_master_bus_handle_t bus, uint8_t addr)
{
    touch_dev_t *t = calloc(1, sizeof(touch_dev_t));
    if (!t) return NULL;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &t->dev));
    return t;
}

#if defined CONFIG_SCREEN_TYPE_HI8561
static void touch_update(touch_dev_t *t)
{
    uint8_t reg = 0x02;
    uint8_t buf[3] = {0};
    if (i2c_master_transmit_receive(t->dev, &reg, 1, buf, 3, pdMS_TO_TICKS(100)) == ESP_OK) {
        t->finger_count = buf[0] & 0x0F;
    } else {
        t->finger_count = 0;
    }
}
#elif defined CONFIG_SCREEN_TYPE_RM69A10
static void touch_update(touch_dev_t *t)
{
    uint8_t cmd[2] = {0x41, 0x44};
    uint8_t buf[2] = {0};
    if (i2c_master_transmit_receive(t->dev, cmd, 2, buf, 2, pdMS_TO_TICKS(100)) == ESP_OK) {
        t->finger_count = buf[0];
    } else {
        t->finger_count = 0;
    }
}
#endif

/* ---------- Board context ---------- */

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t codec_i2c_bus;
    i2c_master_bus_handle_t xl_i2c_bus;
    xl9535_t *xl9535;
    touch_dev_t *touch;

    audio_codec_t *codec;
    display_t *display;

    esp_lcd_panel_handle_t mipi_dpi_panel;

    board_btn_t *boot_button;
} tdisplayp4_ctx_t;

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

static void touch_task(void *arg)
{
    tdisplayp4_ctx_t *ctx = (tdisplayp4_ctx_t *)arg;

    bool touch_flag = false;
    bool touch_lock_flag = true;
    size_t first_touch_time = 0;
    bool waiting_for_second_tap = false;

    while (1) {
        touch_update(ctx->touch);

        if (ctx->touch->finger_count > 0) {
            if (!touch_flag) {
                touch_lock_flag = false;
            }
            touch_flag = true;
        } else {
            touch_flag = false;
        }

        if (!touch_lock_flag) {
            size_t current_time = (size_t)(esp_timer_get_time() / 1000);

            if (!waiting_for_second_tap) {
                first_touch_time = current_time;
                waiting_for_second_tap = true;
            } else {
                if ((current_time - first_touch_time) <= 500) {
                    app_context_t *app = app_get_context();
                    if (app) {
                        if (app_get_device_state(app) == kDeviceStateStarting) {
                            /* skip */
                        } else {
                            app_toggle_chat(app);
                        }
                    }
                    waiting_for_second_tap = false;
                    first_touch_time = 0;
                } else {
                    first_touch_time = current_time;
                }
            }

            touch_lock_flag = true;
        }

        if (waiting_for_second_tap) {
            size_t current_time = (size_t)(esp_timer_get_time() / 1000);
            if ((current_time - first_touch_time) > 500) {
                waiting_for_second_tap = false;
                first_touch_time = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void init_codec_i2c(tdisplayp4_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)P4_ES8311_SDA,
        .scl_io_num = (gpio_num_t)P4_ES8311_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->codec_i2c_bus));
}

static void init_xl9535_i2c(tdisplayp4_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = (gpio_num_t)P4_IIC_1_SDA,
        .scl_io_num = (gpio_num_t)P4_IIC_1_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->xl_i2c_bus));
}

static void init_xl9535(tdisplayp4_ctx_t *ctx)
{
    ctx->xl9535 = xl9535_create(ctx->xl_i2c_bus, P4_XL9535_ADDR);
    xl9535_t *x = ctx->xl9535;

    xl9535_pin_mode_output(x, XL_ESP32P4_VCCA_POWER_EN);
    xl9535_pin_mode_output(x, XL_5_0_V_POWER_EN);
    xl9535_pin_mode_output(x, XL_3_3_V_POWER_EN);
    xl9535_pin_mode_output(x, XL_GPS_WAKE_UP);
    xl9535_pin_write(x, XL_GPS_WAKE_UP, false);
    xl9535_pin_mode_output(x, XL_ESP32C6_EN);
    xl9535_pin_write(x, XL_ESP32C6_EN, false);

    xl9535_pin_write(x, XL_ESP32P4_VCCA_POWER_EN, false);

    xl9535_pin_write(x, XL_5_0_V_POWER_EN, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9535_pin_write(x, XL_5_0_V_POWER_EN, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9535_pin_write(x, XL_5_0_V_POWER_EN, true);
    vTaskDelay(pdMS_TO_TICKS(10));

    xl9535_pin_write(x, XL_3_3_V_POWER_EN, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9535_pin_write(x, XL_3_3_V_POWER_EN, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9535_pin_write(x, XL_3_3_V_POWER_EN, false);
    vTaskDelay(pdMS_TO_TICKS(10));

    xl9535_pin_write(x, XL_ESP32C6_EN, true);
    vTaskDelay(pdMS_TO_TICKS(100));
    xl9535_pin_write(x, XL_ESP32C6_EN, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    xl9535_pin_write(x, XL_ESP32C6_EN, true);
    vTaskDelay(pdMS_TO_TICKS(100));

    vTaskDelay(pdMS_TO_TICKS(1000));
}

#if SOC_MIPI_DSI_SUPPORTED
static void init_lcd(tdisplayp4_ctx_t *ctx)
{
    xl9535_t *x = ctx->xl9535;

    xl9535_pin_mode_output(x, XL_SCREEN_RST);
    xl9535_pin_write(x, XL_SCREEN_RST, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9535_pin_write(x, XL_SCREEN_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9535_pin_write(x, XL_SCREEN_RST, true);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_ldo_channel_handle_t ldo_handle = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = 3, .voltage_mv = 1800 };
    esp_ldo_acquire_channel(&ldo_cfg, &ldo_handle);

    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = P4_DATA_LANES,
        .lane_bit_rate_mbps = P4_LANE_RATE,
    };
    esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    esp_lcd_dbi_io_config_t dbi_io_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_io_config, &mipi_dbi_io);

    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = P4_DSI_DPI_CLK,
        .pixel_format = P4_SCREEN_PIXEL_FMT,
        .num_fbs = 0,
        .video_timing = {
            .h_size = P4_SCREEN_WIDTH,
            .v_size = P4_SCREEN_HEIGHT,
            .hsync_pulse_width = P4_DSI_HSYNC,
            .hsync_back_porch = P4_DSI_HBP,
            .hsync_front_porch = P4_DSI_HFP,
            .vsync_pulse_width = P4_DSI_VSYNC,
            .vsync_back_porch = P4_DSI_VBP,
            .vsync_front_porch = P4_DSI_VFP,
        },
        .flags = {
            .use_dma2d = true,
        },
    };

#if defined CONFIG_SCREEN_TYPE_HI8561
    hi8561_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    esp_lcd_panel_dev_config_t dev_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = P4_SCREEN_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    esp_lcd_new_panel_hi8561(mipi_dbi_io, &dev_config, &ctx->mipi_dpi_panel);
#elif defined CONFIG_SCREEN_TYPE_RM69A10
    rm69a10_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    esp_lcd_panel_dev_config_t dev_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = P4_SCREEN_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    esp_lcd_new_panel_rm69a10(mipi_dbi_io, &dev_config, &ctx->mipi_dpi_panel);
#endif

    esp_lcd_panel_init(ctx->mipi_dpi_panel);

#if defined CONFIG_SCREEN_TYPE_HI8561
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {
        .gpio_num = P4_HI8561_SCREEN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch_conf);
#endif

    ctx->display = mipi_lcd_display_create(mipi_dbi_io, ctx->mipi_dpi_panel,
        P4_SCREEN_WIDTH, P4_SCREEN_HEIGHT, 0, 0, false, false, false);
}
#endif

static void init_touch(tdisplayp4_ctx_t *ctx)
{
    xl9535_t *x = ctx->xl9535;

    xl9535_pin_mode_output(x, XL_TOUCH_RST);
    xl9535_pin_write(x, XL_TOUCH_RST, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9535_pin_write(x, XL_TOUCH_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9535_pin_write(x, XL_TOUCH_RST, true);
    vTaskDelay(pdMS_TO_TICKS(10));

    ctx->touch = touch_dev_create(ctx->xl_i2c_bus, P4_TOUCH_ADDR);

    xTaskCreate(touch_task, "tp", 2 * 1024, ctx, 5, NULL);
}

static void init_buttons(tdisplayp4_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = P4_BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *board_get_board_type(board_desc_t *self)
{
    (void)self;
    return "LilyGo T-Display P4";
}

static void *board_get_audio_codec(board_desc_t *self)
{
    tdisplayp4_ctx_t *ctx = (tdisplayp4_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->codec_i2c_bus, I2C_NUM_0,
            P4_AUDIO_INPUT_SAMPLE_RATE, P4_AUDIO_OUTPUT_SAMPLE_RATE,
            P4_ES8311_MCLK, P4_ES8311_BCLK, P4_ES8311_WS_LRCK,
            P4_ES8311_DAC_DATA, P4_ES8311_ADC_DATA,
            GPIO_NUM_NC, P4_ES8311_ADDR, true, false);
    }
    return ctx->codec;
}

static void *board_get_display(board_desc_t *self)
{
    tdisplayp4_ctx_t *ctx = (tdisplayp4_ctx_t *)self;
    return ctx->display;
}

static void *board_get_backlight(board_desc_t *self)
{
    (void)self;
    return NULL;
}

static void board_destroy(board_desc_t *self)
{
    tdisplayp4_ctx_t *ctx = (tdisplayp4_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx->xl9535);
    free(ctx->touch);
    free(ctx);
}

#if defined CONFIG_SCREEN_TYPE_HI8561
static void set_hi8561_backlight(uint8_t brightness)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
#endif

board_desc_t *create_board_desc(void)
{
    tdisplayp4_ctx_t *ctx = calloc(1, sizeof(tdisplayp4_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = board_get_board_type;
    ctx->base.get_audio_codec = board_get_audio_codec;
    ctx->base.get_display = board_get_display;
    ctx->base.get_backlight = board_get_backlight;
    ctx->base.destroy = board_destroy;

    init_codec_i2c(ctx);
    init_xl9535_i2c(ctx);
    init_xl9535(ctx);
#if SOC_MIPI_DSI_SUPPORTED
    init_lcd(ctx);
#endif
    init_touch(ctx);
    init_buttons(ctx);

#if defined CONFIG_SCREEN_TYPE_HI8561
    set_hi8561_backlight(100);
#elif defined CONFIG_SCREEN_TYPE_RM69A10
    if (ctx->mipi_dpi_panel) {
        set_rm69a10_brightness(ctx->mipi_dpi_panel, 255);
    }
#endif

    return &ctx->base;
}
