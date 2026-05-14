#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "c_api/codec_c_api.h"
#include "display/display.h"
#include "led/led.h"
#include "boards/common/backlight.h"
#include "boards/common/i2c_device.h"
#include "device_state.h"
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>
#include <esp_lcd_io_spi.h>
#include "c_api/board_c_api.h"
#include "backlight.h"

#define TAG "AtomS3R+EchoPyramid"

#define PYRAMID_SI5351_ADDR  0x60
#define PYRAMID_STM32_ADDR   0x1A
#define PYRAMID_AW87559_ADDR 0x5B
#define PYRAMID_POWER_ON_RETRY_COUNT    20
#define PYRAMID_POWER_ON_RETRY_DELAY_MS 250

#define STM32_SPK_RESTART_REG_ADDR       0xA0
#define STM32_RGB1_BRIGHTNESS_REG_ADDR   0x10
#define STM32_RGB2_BRIGHTNESS_REG_ADDR   0x11
#define STM32_RGB1_STATUS_REG_ADDR       0x20
#define STM32_RGB2_STATUS_REG_ADDR       0x60

#define AW87559_REG_SYSCTRL 0x01
#define AW87559_REG_PAGR    0x06
#define AW87559_ID           0x5A
#define AW87559_SYS_EN_SW_MASK    (1 << 6)
#define AW87559_SYS_EN_BOOST_MASK (1 << 4)
#define AW87559_SYS_EN_PA_MASK    (1 << 3)
#define AW87559_GAIN_16_5DB 11

#define LP5562_ADDR 0x30

static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb2, (uint8_t[]){0x2f}, 1, 0},
    {0xb3, (uint8_t[]){0x03}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x01}, 1, 0},
    {0xac, (uint8_t[]){0xcb}, 1, 0},
    {0xab, (uint8_t[]){0x0e}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x19}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xe8, (uint8_t[]){0x24}, 1, 0},
    {0xe9, (uint8_t[]){0x48}, 1, 0},
    {0xea, (uint8_t[]){0x22}, 1, 0},
    {0xc6, (uint8_t[]){0x30}, 1, 0},
    {0xc7, (uint8_t[]){0x18}, 1, 0},
    {0xf0,
     (uint8_t[]){0x1f, 0x28, 0x04, 0x3e, 0x2a, 0x2e, 0x20, 0x00, 0x0c, 0x06,
                 0x00, 0x1c, 0x1f, 0x0f},
     14, 0},
    {0xf1,
     (uint8_t[]){0x00, 0x2d, 0x2f, 0x3c, 0x6f, 0x1c, 0x0b, 0x00, 0x00, 0x00,
                 0x07, 0x0d, 0x11, 0x0f},
     14, 0},
};

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    i2c_master_bus_handle_t i2c_bus_internal;

    i2c_device_t *si5351;
    i2c_device_t *aw87559;
    i2c_device_t *stm32;
    i2c_device_t *lp5562;

    led_t led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;

    bool is_pyramid_connected;
} pyramid_ctx_t;

/* ── forward declarations ───────────────────────────────────────────── */
static void init_buttons(pyramid_ctx_t *ctx);

/* ── Si5351 helper ──────────────────────────────────────────────────── */

static void si5351_write_regs(i2c_device_t *dev, uint8_t reg,
                              const uint8_t *data, size_t length)
{
    i2c_master_dev_handle_t handle = i2c_device_get_dev_handle(dev);
    uint8_t buffer[9] = {0};
    buffer[0] = reg;
    for (size_t i = 0; i < length; ++i)
        buffer[i + 1] = data[i];
    ESP_ERROR_CHECK(i2c_master_transmit(handle, buffer, length + 1, 100));
}

static void si5351_set_pll(i2c_device_t *dev, uint32_t pll_freq, uint32_t ms_div)
{
    static const uint32_t kXtalFreq = 27000000UL;

    uint32_t a = pll_freq / kXtalFreq;
    uint32_t rest = pll_freq % kXtalFreq;
    uint32_t c = 1000000UL;
    uint32_t b = (rest * c) / kXtalFreq;

    uint32_t p1 = 128 * a + (128 * b) / c - 512;
    uint32_t p2 = 128 * b - c * ((128 * b) / c);
    uint32_t p3 = c;

    i2c_device_write_reg(dev, 3, 0xFF);

    uint8_t pll_buf[8] = {
        (uint8_t)((p3 >> 8) & 0xFF),
        (uint8_t)(p3 & 0xFF),
        (uint8_t)((p1 >> 16) & 0x03),
        (uint8_t)((p1 >> 8) & 0xFF),
        (uint8_t)(p1 & 0xFF),
        (uint8_t)(((p3 >> 12) & 0xF0) | ((p2 >> 16) & 0x0F)),
        (uint8_t)((p2 >> 8) & 0xFF),
        (uint8_t)(p2 & 0xFF),
    };
    si5351_write_regs(dev, 26, pll_buf, sizeof(pll_buf));

    uint32_t ms_p1 = 128 * ms_div - 512;
    uint8_t ms_buf[8] = {
        0x00,
        0x01,
        (uint8_t)((ms_p1 >> 16) & 0x03),
        (uint8_t)((ms_p1 >> 8) & 0xFF),
        (uint8_t)(ms_p1 & 0xFF),
        0x00,
        0x00,
        0x00,
    };
    si5351_write_regs(dev, 50, ms_buf, sizeof(ms_buf));

    i2c_device_write_reg(dev, 17, 0x4F);
    i2c_device_write_reg(dev, 16, 0x80);
    i2c_device_write_reg(dev, 18, 0x80);
    i2c_device_write_reg(dev, 177, 0xA0);
    vTaskDelay(pdMS_TO_TICKS(10));
    i2c_device_write_reg(dev, 3, 0xFD);

    ESP_LOGI(TAG, "Si5351 CLK1 set to %lu Hz", (unsigned long)(pll_freq / ms_div));
}

static void si5351_init(i2c_device_t *dev)
{
    i2c_device_write_reg(dev, 3, 0xFF);
    i2c_device_write_reg(dev, 16, 0x80);
    i2c_device_write_reg(dev, 17, 0x80);
    i2c_device_write_reg(dev, 18, 0x80);
    i2c_device_write_reg(dev, 183, 0xC0);
}

static void si5351_set_mclk(i2c_device_t *dev, uint32_t sample_rate)
{
    if (sample_rate == 24000) {
        si5351_set_pll(dev, 884736000UL, 144);
    } else if (sample_rate == 16000) {
        si5351_set_pll(dev, 884736000UL, 216);
    } else if (sample_rate == 44100) {
        si5351_set_pll(dev, 903168000UL, 80);
    } else if (sample_rate == 48000) {
        si5351_set_pll(dev, 884736000UL, 72);
    } else {
        ESP_LOGW(TAG, "Unsupported Si5351 sample rate: %lu", (unsigned long)sample_rate);
    }
}

/* ── AW87559 helper ─────────────────────────────────────────────────── */

static void aw87559_update_bits(i2c_device_t *dev, uint8_t reg,
                                uint8_t mask, uint8_t value)
{
    uint8_t v = i2c_device_read_reg(dev, reg);
    v &= ~mask;
    v |= value & mask;
    i2c_device_write_reg(dev, reg, v);
}

static void aw87559_init(i2c_device_t *dev)
{
    uint8_t id = i2c_device_read_reg(dev, 0x00);
    if (id != AW87559_ID) {
        ESP_LOGW(TAG, "Unexpected AW87559 ID: 0x%02x", id);
    }
    aw87559_update_bits(dev, AW87559_REG_SYSCTRL, AW87559_SYS_EN_SW_MASK, AW87559_SYS_EN_SW_MASK);
    aw87559_update_bits(dev, AW87559_REG_SYSCTRL, AW87559_SYS_EN_BOOST_MASK, AW87559_SYS_EN_BOOST_MASK);
    aw87559_update_bits(dev, AW87559_REG_SYSCTRL, AW87559_SYS_EN_PA_MASK, AW87559_SYS_EN_PA_MASK);
    aw87559_update_bits(dev, AW87559_REG_PAGR, 0x1F, AW87559_GAIN_16_5DB);
}

/* ── STM32 Pyramid Controller ───────────────────────────────────────── */

static void stm32_set_all_rgb(i2c_device_t *dev, uint8_t channel,
                              uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t base = (channel == 1) ? STM32_RGB1_STATUS_REG_ADDR
                                        : STM32_RGB2_STATUS_REG_ADDR;
    i2c_master_dev_handle_t handle = i2c_device_get_dev_handle(dev);

    for (int page = 0; page < 4; ++page) {
        uint8_t reg = base + (uint8_t)(page * 0x10);
        uint8_t payload[1 + 16] = {0};
        payload[0] = reg;
        for (int i = 0; i < 4; ++i) {
            payload[1 + i * 4 + 0] = b;
            payload[1 + i * 4 + 1] = g;
            payload[1 + i * 4 + 2] = r;
            payload[1 + i * 4 + 3] = 0x00;
        }
        ESP_ERROR_CHECK(i2c_master_transmit(handle, payload, sizeof(payload), 100));
    }
}

static void stm32_set_status_color(i2c_device_t *dev, uint8_t r, uint8_t g, uint8_t b)
{
    stm32_set_all_rgb(dev, 1, r, g, b);
    stm32_set_all_rgb(dev, 2, r, g, b);
}

static void stm32_init(i2c_device_t *dev)
{
    i2c_device_write_reg(dev, STM32_SPK_RESTART_REG_ADDR, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    i2c_device_write_reg(dev, STM32_RGB1_BRIGHTNESS_REG_ADDR, 100);
    i2c_device_write_reg(dev, STM32_RGB2_BRIGHTNESS_REG_ADDR, 100);
    stm32_set_all_rgb(dev, 1, 0, 0, 64);
    stm32_set_all_rgb(dev, 2, 0, 0, 64);
}

/* ── Pyramid status LED (delegates to STM32 RGB) ───────────────────── */

static void pyramid_led_on_state_changed(led_t *led, DeviceState state,
                                         bool is_voice_detected)
{
    (void)is_voice_detected;
    pyramid_ctx_t *ctx = (pyramid_ctx_t *)((char *)led -
                          offsetof(pyramid_ctx_t, led));
    if (!ctx->stm32) return;

    switch (state) {
        case kDeviceStateListening:
            stm32_set_status_color(ctx->stm32, 0, 64, 0);
            break;
        case kDeviceStateSpeaking:
            stm32_set_status_color(ctx->stm32, 64, 0, 0);
            break;
        default:
            stm32_set_status_color(ctx->stm32, 0, 0, 64);
            break;
    }
}

static const led_ops_t pyramid_led_ops = {
    .on_state_changed = pyramid_led_on_state_changed,
    .destroy = NULL,
};

/* ── LP5562 / backlight ─────────────────────────────────────────────── */

static void lp5562_set_brightness_impl(void *impl_ctx, uint8_t brightness)
{
    i2c_device_t *dev = (i2c_device_t *)impl_ctx;
    if (!dev) return;
    uint8_t mapped = brightness * 255 / 100;
    i2c_device_write_reg(dev, 0x0E, mapped);
}

/* ── Callbacks ──────────────────────────────────────────────────────── */

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

/* ── Init helpers ───────────────────────────────────────────────────── */

static void init_i2c(pyramid_ctx_t *ctx)
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

    i2c_bus_cfg.i2c_port = I2C_NUM_0;
    i2c_bus_cfg.sda_io_num = GPIO_NUM_45;
    i2c_bus_cfg.scl_io_num = GPIO_NUM_0;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->i2c_bus_internal));
}

static void i2c_detect(pyramid_ctx_t *ctx)
{
    ctx->is_pyramid_connected = false;
    bool has_es8311 = false;
    bool has_es7210 = false;
    bool has_si5351 = false;
    bool has_stm32 = false;
    bool has_aw87559 = false;
    uint8_t address;

    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
    for (int i = 0; i < 128; i += 16) {
        printf("%02x: ", i);
        for (int j = 0; j < 16; j++) {
            fflush(stdout);
            address = i + j;
            esp_err_t ret = i2c_master_probe(ctx->i2c_bus, address, pdMS_TO_TICKS(200));
            if (ret == ESP_OK) {
                printf("%02x ", address);
                if (address == (AUDIO_CODEC_ES8311_ADDR >> 1)) {
                    has_es8311 = true;
                } else if (address == (AUDIO_CODEC_ES7210_ADDR >> 1)) {
                    has_es7210 = true;
                } else if (address == PYRAMID_SI5351_ADDR) {
                    has_si5351 = true;
                } else if (address == PYRAMID_STM32_ADDR) {
                    has_stm32 = true;
                } else if (address == PYRAMID_AW87559_ADDR) {
                    has_aw87559 = true;
                }
            } else if (ret == ESP_ERR_TIMEOUT) {
                printf("UU ");
            } else {
                printf("-- ");
            }
        }
        printf("\r\n");
    }

    ctx->is_pyramid_connected = has_es8311 && has_es7210 && has_si5351 && has_stm32 && has_aw87559;
}

static void wait_for_pyramid_connection(pyramid_ctx_t *ctx)
{
    for (int attempt = 0; attempt < PYRAMID_POWER_ON_RETRY_COUNT; ++attempt) {
        i2c_detect(ctx);
        if (ctx->is_pyramid_connected) {
            if (attempt > 0) {
                ESP_LOGI(TAG, "Echo Pyramid detected after %d retries", attempt);
            }
            return;
        }

        ESP_LOGW(TAG, "Echo Pyramid not ready, retrying (%d/%d)",
            attempt + 1, PYRAMID_POWER_ON_RETRY_COUNT);
        vTaskDelay(pdMS_TO_TICKS(PYRAMID_POWER_ON_RETRY_DELAY_MS));
    }
}

static void init_lp5562(pyramid_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init LP5562");
    ctx->lp5562 = i2c_device_create(ctx->i2c_bus_internal, LP5562_ADDR);
    i2c_device_write_reg(ctx->lp5562, 0x00, 0x40);
    i2c_device_write_reg(ctx->lp5562, 0x08, 0x01);
    i2c_device_write_reg(ctx->lp5562, 0x70, 0x00);

    uint8_t data = i2c_device_read_reg(ctx->lp5562, 0x08);
    data = data | 0x40;
    i2c_device_write_reg(ctx->lp5562, 0x08, data);

    backlight_impl_t bl_impl = {
        .set_brightness = lp5562_set_brightness_impl,
        .destroy = NULL,
        .impl_ctx = ctx->lp5562,
    };
    ctx->backlight = backlight_create(&bl_impl);
}

static void init_spi(pyramid_ctx_t *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .mosi_io_num = GPIO_NUM_21,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = GPIO_NUM_15,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_gc9107_display(pyramid_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init GC9107 display");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_NUM_14,
        .dc_gpio_num = GPIO_NUM_42,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    gc9a01_vendor_config_t gc9107_vendor_config = {
        .init_cmds = gc9107_lcd_init_cmds,
        .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_48,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
        .vendor_config = &gc9107_vendor_config,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ctx->display = spi_lcd_display_create(io_handle, panel_handle,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void check_pyramid_connection(pyramid_ctx_t *ctx)
{
    if (ctx->is_pyramid_connected) {
        return;
    }

    init_lp5562(ctx);
    init_spi(ctx);
    init_gc9107_display(ctx);
    init_buttons(ctx);
    backlight_set_brightness(ctx->backlight, 100, false);

    display_setup_ui(ctx->display);
    display_set_status(ctx->display, "Error");
    display_set_emotion(ctx->display, "triangle_exclamation");
    display_set_chat_message(ctx->display, "system", "Echo Pyramid\nnot connected");

    while (1) {
        ESP_LOGE(TAG, "Echo Pyramid is disconnected");
        vTaskDelay(pdMS_TO_TICKS(1000));

        i2c_detect(ctx);
        if (ctx->is_pyramid_connected) {
            vTaskDelay(pdMS_TO_TICKS(500));
            i2c_detect(ctx);
            if (ctx->is_pyramid_connected) {
                ESP_LOGI(TAG, "Echo Pyramid is reconnected");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
    }
}

static void init_pyramid_devices(pyramid_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init Echo Pyramid devices");

    ctx->si5351 = i2c_device_create(ctx->i2c_bus, PYRAMID_SI5351_ADDR);
    si5351_init(ctx->si5351);
    si5351_set_mclk(ctx->si5351, AUDIO_OUTPUT_SAMPLE_RATE);

    ctx->stm32 = i2c_device_create(ctx->i2c_bus, PYRAMID_STM32_ADDR);
    stm32_init(ctx->stm32);

    ctx->led.ops = &pyramid_led_ops;
    stm32_set_status_color(ctx->stm32, 0, 0, 64);

    ctx->aw87559 = i2c_device_create(ctx->i2c_bus, PYRAMID_AW87559_ADDR);
    aw87559_init(ctx->aw87559);
}

static void init_buttons(pyramid_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

/* ── board_desc vtable ──────────────────────────────────────────────── */

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_led(board_desc_t *self)
{
    pyramid_ctx_t *ctx = (pyramid_ctx_t *)self;
    return &ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    pyramid_ctx_t *ctx = (pyramid_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = box_codec_create(
            ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_GPIO_PA, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    pyramid_ctx_t *ctx = (pyramid_ctx_t *)self;
    return ctx->display;
}

static void *get_backlight(board_desc_t *self)
{
    pyramid_ctx_t *ctx = (pyramid_ctx_t *)self;
    return ctx->backlight;
}

static void destroy(board_desc_t *self)
{
    pyramid_ctx_t *ctx = (pyramid_ctx_t *)self;
    if (ctx->backlight) {
        backlight_destroy(ctx->backlight);
        ctx->backlight = NULL;
    }
    board_btn_delete(ctx->boot_button);
    if (ctx->si5351) i2c_device_destroy(ctx->si5351);
    if (ctx->aw87559) i2c_device_destroy(ctx->aw87559);
    if (ctx->stm32)  i2c_device_destroy(ctx->stm32);
    if (ctx->lp5562) i2c_device_destroy(ctx->lp5562);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    pyramid_ctx_t *ctx = calloc(1, sizeof(pyramid_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    init_i2c(ctx);
    wait_for_pyramid_connection(ctx);
    check_pyramid_connection(ctx);
    init_pyramid_devices(ctx);
    init_lp5562(ctx);
    init_spi(ctx);
    init_gc9107_display(ctx);
    init_buttons(ctx);
    backlight_restore_brightness(ctx->backlight);

    return &ctx->base;
}
