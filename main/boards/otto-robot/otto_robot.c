/*
 * Otto Robot board - Converted from C++ to C.
 * Uses the create_board_desc() pattern (see compact_wifi_board.c).
 *
 * Camera support (EspVideo) and power manager (PowerManager) are C++ only
 * and therefore omitted; get_camera returns NULL, get_battery_level returns false.
 */

#include "board_defs.h"
#include "button.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "audio/codecs/no_audio_codec.h"
#include "boards/common/backlight.h"
#include "device_state.h"

/* config.h uses `constexpr` for two struct instances; map it to `static const` for C */
#define constexpr static const
#include "config.h"
/* NOLINTBEGIN  – constexpr is redefined only to absorb config.h */

#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "c_api/board_c_api.h"
#include "backlight.h"

#define TAG "OttoRobot"

/* ── External functions (from other .c files in this board) ── */
extern void InitializeOttoController(int ll, int rl, int lf, int rf, int lh, int rh);
extern bool ws_control_server_start(int port);

/* ── Board context ─────────────────────────────────────── */
typedef struct {
    board_desc_t base;

    display_t *display;
    audio_codec_t *codec;
    backlight_t *backlight;
    board_btn_t *boot_button;
    led_t *led;

    i2c_master_bus_handle_t i2c_bus;
    bool has_camera;
    enum OttoCameraType camera_type;

    /* Copy of the resolved hardware config */
    struct HardwareConfig hw;
} otto_robot_ctx_t;

/* ── Hardware version detection ────────────────────────── */
static bool detect_hardware_version(otto_robot_ctx_t *ctx) {
    ledc_timer_config_t lt = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_2_BIT,
        .timer_num      = LEDC_TIMER,
        .freq_hz        = CAMERA_XCLK_FREQ,
        .clk_cfg        = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&lt) != ESP_OK) return false;

    ledc_channel_config_t lc = {
        .gpio_num   = CAMERA_XCLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER,
        .duty       = 2,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&lc) != ESP_OK) return false;

    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = CAMERA_VERSION_CONFIG.i2c_sda_pin,
        .scl_io_num = CAMERA_VERSION_CONFIG.i2c_scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };

    if (i2c_new_master_bus(&bus_cfg, &ctx->i2c_bus) != ESP_OK) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
        return false;
    }

    const uint8_t camera_addrs[] = {0x30, 0x3C, 0x21, 0x60};
    bool found = false;
    uint16_t pid = 0;

    for (size_t a = 0; a < sizeof(camera_addrs); a++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = camera_addrs[a],
            .scl_speed_hz    = 100000,
        };
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(ctx->i2c_bus, &dev_cfg, &dev) != ESP_OK) continue;

        /* Try OV2640-style read */
        uint8_t reg8 = 0x0A;
        uint8_t data[2] = {0, 0};
        if (i2c_master_transmit_receive(dev, &reg8, 1, data, 2, 200) == ESP_OK &&
            (data[0] || data[1])) {
            pid = (uint16_t)((data[0] << 8) | data[1]);
            ESP_LOGI(TAG, "检测到摄像头 (OV2640方式) PID=0x%04X (地址=0x%02X)", pid, camera_addrs[a]);
            found = true;
            i2c_master_bus_rm_device(dev);
            break;
        }

        /* Try OV3660-style 16-bit register read */
        uint8_t rh[2] = {0x30, 0x0A};
        uint8_t rl2[2] = {0x30, 0x0B};
        uint8_t ph = 0, pl = 0;
        if (i2c_master_transmit_receive(dev, rh, 2, &ph, 1, 200) == ESP_OK &&
            i2c_master_transmit_receive(dev, rl2, 2, &pl, 1, 200) == ESP_OK) {
            pid = (uint16_t)((ph << 8) | pl);
            if (pid) {
                ESP_LOGI(TAG, "检测到摄像头 (OV3660方式) PID=0x%04X (地址=0x%02X)", pid, camera_addrs[a]);
                found = true;
                i2c_master_bus_rm_device(dev);
                break;
            }
        }
        i2c_master_bus_rm_device(dev);
    }

    if (!found) {
        i2c_del_master_bus(ctx->i2c_bus);
        ctx->i2c_bus = NULL;
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
        ctx->camera_type = OTTO_CAMERA_NONE;
    } else {
        if (pid == OV2640_PID_1 || pid == OV2640_PID_2) {
            ctx->camera_type = OTTO_CAMERA_OV2640;
            ESP_LOGI(TAG, "摄像头类型: OV2640 (PID=0x%04X)", pid);
        } else if (pid == OV3660_PID) {
            ctx->camera_type = OTTO_CAMERA_OV3660;
            ESP_LOGI(TAG, "摄像头类型: OV3660 (PID=0x%04X)", pid);
        } else {
            ctx->camera_type = OTTO_CAMERA_UNKNOWN;
            ESP_LOGW(TAG, "未知摄像头类型，PID=0x%04X", pid);
        }
    }
    return found;
}

/* ── SPI bus ───────────────────────────────────────────── */
static void init_spi(const struct HardwareConfig *hw) {
    spi_bus_config_t buscfg = {0};
    buscfg.mosi_io_num = hw->display_mosi_pin;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = hw->display_clk_pin;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

/* ── LCD display ───────────────────────────────────────── */
static void init_lcd_display(otto_robot_ctx_t *ctx) {
    const struct HardwareConfig *hw = &ctx->hw;

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {0};
    io_config.cs_gpio_num     = hw->display_cs_pin;
    io_config.dc_gpio_num     = hw->display_dc_pin;
    io_config.spi_mode        = DISPLAY_SPI_MODE;
    io_config.pclk_hz         = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits    = 8;
    io_config.lcd_param_bits  = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {0};
    panel_config.reset_gpio_num  = hw->display_rst_pin;
    panel_config.rgb_ele_order   = DISPLAY_RGB_ORDER;
    panel_config.bits_per_pixel  = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

/* ── Audio codec ───────────────────────────────────────── */
static void init_audio_codec(otto_robot_ctx_t *ctx) {
    const struct HardwareConfig *hw = &ctx->hw;
    if (hw->audio_use_simplex) {
        ctx->codec = no_audio_codec_simplex_create(
            hw->audio_input_sample_rate, hw->audio_output_sample_rate,
            hw->audio_i2s_spk_gpio_bclk, hw->audio_i2s_spk_gpio_lrck,
            hw->audio_i2s_spk_gpio_dout,
            hw->audio_i2s_mic_gpio_sck,  hw->audio_i2s_mic_gpio_ws,
            hw->audio_i2s_mic_gpio_din);
    } else {
        ctx->codec = no_audio_codec_duplex_create(
            hw->audio_input_sample_rate, hw->audio_output_sample_rate,
            hw->audio_i2s_gpio_bclk, hw->audio_i2s_gpio_ws,
            hw->audio_i2s_gpio_dout, hw->audio_i2s_gpio_din);
    }
}

/* ── Button ────────────────────────────────────────────── */
static void on_boot_click(void *ud) {
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) {
        board_enter_wifi_config_mode(board_get_instance());
        return;
    }
    app_toggle_chat(app);
}

static void init_buttons(otto_robot_ctx_t *ctx) {
    board_btn_gpio_cfg_t cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

/* ── board_desc_t vtable ───────────────────────────────── */
static const char *orb_get_board_type(board_desc_t *self) {
    (void)self;
    return "otto-robot";
}

static void *orb_get_led(board_desc_t *self) {
    (void)self;
    return NULL;
}

static void *orb_get_audio_codec(board_desc_t *self) {
    otto_robot_ctx_t *ctx = (otto_robot_ctx_t *)self;
    return ctx->codec;
}

static void *orb_get_display(board_desc_t *self) {
    otto_robot_ctx_t *ctx = (otto_robot_ctx_t *)self;
    return ctx->display;
}

static void *orb_get_backlight(board_desc_t *self) {
    otto_robot_ctx_t *ctx = (otto_robot_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(
            ctx->hw.display_backlight_pin, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void *orb_get_camera(board_desc_t *self) {
    (void)self;
    return NULL;
}

static bool orb_get_battery_level(board_desc_t *self, int *level, bool *charging, bool *discharging) {
    (void)self;
    *level = 0;
    *charging = false;
    *discharging = false;
    return false;
}

static void orb_destroy(board_desc_t *self) {
    otto_robot_ctx_t *ctx = (otto_robot_ctx_t *)self;
    if (ctx->backlight) {
        backlight_destroy(ctx->backlight);
        ctx->backlight = NULL;
    }
    if (ctx->boot_button) board_btn_delete(ctx->boot_button);
    free(ctx);
}

/* ── create_board_desc ─────────────────────────────────── */
board_desc_t *create_board_desc(void) {
    otto_robot_ctx_t *ctx = (otto_robot_ctx_t *)calloc(1, sizeof(otto_robot_ctx_t));
    if (!ctx) return NULL;

    /* ── Hardware version detection ── */
#if OTTO_HARDWARE_VERSION == OTTO_VERSION_AUTO
    ctx->has_camera = detect_hardware_version(ctx);
    ESP_LOGI(TAG, "自动检测硬件版本: %s", ctx->has_camera ? "摄像头版" : "无摄像头版");
#elif OTTO_HARDWARE_VERSION == OTTO_VERSION_CAMERA
    ctx->has_camera = detect_hardware_version(ctx);
    if (!ctx->has_camera) {
        ctx->has_camera = true;
        ctx->camera_type = OTTO_CAMERA_UNKNOWN;
        ESP_LOGW(TAG, "强制使用摄像头版本配置，但未能检测到摄像头类型");
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port   = I2C_NUM_0,
            .sda_io_num = CAMERA_VERSION_CONFIG.i2c_sda_pin,
            .scl_io_num = CAMERA_VERSION_CONFIG.i2c_scl_pin,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority     = 0,
            .trans_queue_depth = 0,
            .flags = { .enable_internal_pullup = 1 },
        };
        i2c_new_master_bus(&bus_cfg, &ctx->i2c_bus);
    } else {
        ESP_LOGI(TAG, "强制使用摄像头版本配置");
    }
#elif OTTO_HARDWARE_VERSION == OTTO_VERSION_NO_CAMERA
    ctx->has_camera  = false;
    ctx->camera_type = OTTO_CAMERA_NONE;
    ESP_LOGI(TAG, "强制使用无摄像头版本配置");
#else
#error "OTTO_HARDWARE_VERSION 设置无效"
#endif

    ctx->hw = ctx->has_camera ? CAMERA_VERSION_CONFIG : NON_CAMERA_VERSION_CONFIG;

    /* ── Peripherals ── */
    init_spi(&ctx->hw);
    init_lcd_display(ctx);
    init_buttons(ctx);
    init_audio_codec(ctx);

    /* ── Otto controller (servo motors + MCP tools) ── */
    InitializeOttoController(
        (int)ctx->hw.left_leg_pin,  (int)ctx->hw.right_leg_pin,
        (int)ctx->hw.left_foot_pin, (int)ctx->hw.right_foot_pin,
        (int)ctx->hw.left_hand_pin, (int)ctx->hw.right_hand_pin);

    /* ── WebSocket control server ── */
    ws_control_server_start(8080);

    /* ── Backlight ── */
    ctx->backlight = pwm_backlight_create(
        ctx->hw.display_backlight_pin, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    backlight_restore_brightness(ctx->backlight);

    /* ── Fill vtable ── */
    ctx->base.kind              = BOARD_KIND_WIFI;
    ctx->base.get_board_type    = orb_get_board_type;
    ctx->base.get_led           = orb_get_led;
    ctx->base.get_audio_codec   = orb_get_audio_codec;
    ctx->base.get_display       = orb_get_display;
    ctx->base.get_backlight     = orb_get_backlight;
    ctx->base.get_camera        = orb_get_camera;
    ctx->base.get_battery_level = orb_get_battery_level;
    ctx->base.destroy           = orb_destroy;

    return &ctx->base;
}
