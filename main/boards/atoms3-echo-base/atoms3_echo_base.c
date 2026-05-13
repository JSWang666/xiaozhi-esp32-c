#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "c_api/codec_c_api.h"
#include "display/display.h"
#include "led/single_led.h"
#include "boards/common/backlight.h"
#include "boards/common/i2c_device.h"
#include "device_state.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>
#include <esp_lcd_io_spi.h>

#define TAG "AtomS3+EchoBase"

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

    led_t *led;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;

    bool is_echo_base_connected;
} atoms3_echo_base_ctx_t;

static void init_buttons(atoms3_echo_base_ctx_t *ctx);

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

static void init_i2c(atoms3_echo_base_ctx_t *ctx)
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

static void i2c_detect(atoms3_echo_base_ctx_t *ctx)
{
    ctx->is_echo_base_connected = false;
    uint8_t echo_base_connected_flag = 0x00;
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
                if (address == 0x18) {
                    echo_base_connected_flag |= 0xF0;
                } else if (address == 0x43) {
                    echo_base_connected_flag |= 0x0F;
                }
            } else if (ret == ESP_ERR_TIMEOUT) {
                printf("UU ");
            } else {
                printf("-- ");
            }
        }
        printf("\r\n");
    }
    ctx->is_echo_base_connected = (echo_base_connected_flag == 0xFF);
}

static void init_spi(atoms3_echo_base_ctx_t *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .mosi_io_num = GPIO_NUM_21,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = GPIO_NUM_17,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_gc9107_display(atoms3_echo_base_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init GC9107 display");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_NUM_15,
        .dc_gpio_num = GPIO_NUM_33,
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
        .reset_gpio_num = GPIO_NUM_34,
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

static void check_echo_base_connection(atoms3_echo_base_ctx_t *ctx)
{
    if (ctx->is_echo_base_connected) {
        return;
    }

    init_spi(ctx);
    init_gc9107_display(ctx);
    init_buttons(ctx);
    backlight_set_brightness(ctx->backlight, 100, false);

    display_setup_ui(ctx->display);
    display_set_status(ctx->display, "Error");
    display_set_emotion(ctx->display, "triangle_exclamation");
    display_set_chat_message(ctx->display, "system", "Echo Base\nnot connected");

    while (1) {
        ESP_LOGE(TAG, "Atomic Echo Base is disconnected");
        vTaskDelay(pdMS_TO_TICKS(1000));

        i2c_detect(ctx);
        if (ctx->is_echo_base_connected) {
            vTaskDelay(pdMS_TO_TICKS(500));
            i2c_detect(ctx);
            if (ctx->is_echo_base_connected) {
                ESP_LOGI(TAG, "Atomic Echo Base is reconnected");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
    }
}

static void init_buttons(atoms3_echo_base_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_led(board_desc_t *self)
{
    atoms3_echo_base_ctx_t *ctx = (atoms3_echo_base_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    atoms3_echo_base_ctx_t *ctx = (atoms3_echo_base_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(
            ctx->i2c_bus, I2C_NUM_1,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_GPIO_PA, AUDIO_CODEC_ES8311_ADDR, false, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    atoms3_echo_base_ctx_t *ctx = (atoms3_echo_base_ctx_t *)self;
    return ctx->display;
}

static void *get_backlight(board_desc_t *self)
{
    atoms3_echo_base_ctx_t *ctx = (atoms3_echo_base_ctx_t *)self;
    return ctx->backlight;
}

static void destroy(board_desc_t *self)
{
    atoms3_echo_base_ctx_t *ctx = (atoms3_echo_base_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    atoms3_echo_base_ctx_t *ctx = calloc(1, sizeof(atoms3_echo_base_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 256);

    init_i2c(ctx);
    i2c_detect(ctx);
    check_echo_base_connection(ctx);
    init_spi(ctx);
    init_gc9107_display(ctx);
    init_buttons(ctx);
    backlight_restore_brightness(ctx->backlight);

    return &ctx->base;
}
