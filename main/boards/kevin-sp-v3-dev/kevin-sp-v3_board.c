#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "audio/codecs/no_audio_codec.h"
#include "led/single_led.h"
#include "backlight.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>
#include <driver/spi_master.h>
#include <esp_camera.h>

#define TAG "kevin-sp-v3"

typedef struct {
    board_desc_t base;
    void *led;
    void *codec;
    void *display;
    void *backlight;
} kevin_sp_v3_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
}

static void on_boot_press_down(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_start_listening(app);
}

static void on_boot_press_up(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_stop_listening(app);
}

static void init_spi(void)
{
    spi_bus_config_t buscfg = {0};
    buscfg.mosi_io_num = 47;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = 21;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void *init_display(void)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {0};
    io_config.cs_gpio_num = 14;
    io_config.dc_gpio_num = 45;
    io_config.spi_mode = 3;
    io_config.pclk_hz = 80 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {0};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

    return spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_camera(void)
{
    camera_config_t config = {0};
    config.pin_pwdn = CAMERA_PIN_PWDN;
    config.pin_reset = CAMERA_PIN_RESET;
    config.pin_xclk = CAMERA_PIN_XCLK;
    config.pin_sccb_sda = -1;
    config.pin_sccb_scl = -1;
    config.pin_d7 = CAMERA_PIN_D7;
    config.pin_d6 = CAMERA_PIN_D6;
    config.pin_d5 = CAMERA_PIN_D5;
    config.pin_d4 = CAMERA_PIN_D4;
    config.pin_d3 = CAMERA_PIN_D3;
    config.pin_d2 = CAMERA_PIN_D2;
    config.pin_d1 = CAMERA_PIN_D1;
    config.pin_d0 = CAMERA_PIN_D0;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;
    config.pin_pclk = CAMERA_PIN_PCLK;
    config.xclk_freq_hz = XCLK_FREQ_HZ;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.sccb_i2c_port = 1;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
    }
}

static const char *get_board_type(board_desc_t *self) { (void)self; return "kevin-sp-v3-dev"; }
static void *get_led(board_desc_t *self) { return ((kevin_sp_v3_ctx_t *)self)->led; }
static void *get_audio_codec(board_desc_t *self) { return ((kevin_sp_v3_ctx_t *)self)->codec; }
static void *get_display(board_desc_t *self) { return ((kevin_sp_v3_ctx_t *)self)->display; }
static void *get_backlight(board_desc_t *self) { return ((kevin_sp_v3_ctx_t *)self)->backlight; }

static void destroy(board_desc_t *self)
{
    kevin_sp_v3_ctx_t *ctx = (kevin_sp_v3_ctx_t *)self;
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    kevin_sp_v3_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    ESP_LOGI(TAG, "Initializing KEVIN_SP_V3 Board");

    ctx->led = single_led_create(BUILTIN_LED_GPIO);
    ctx->codec = no_audio_codec_simplex_create(
        AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
        AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 5000);

    init_spi();
    ctx->display = init_display();
    init_camera();

    board_btn_handle_t boot_btn = board_btn_create_gpio(BOOT_BUTTON_GPIO);
    board_btn_on_click(boot_btn, on_boot_click, NULL);
    board_btn_on_press_down(boot_btn, on_boot_press_down, NULL);
    board_btn_on_press_up(boot_btn, on_boot_press_up, NULL);

    return &ctx->base;
}
