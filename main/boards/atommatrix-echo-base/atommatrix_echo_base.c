#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "led/led.h"
#include "boards/common/i2c_device.h"
#include "device_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include "c_api/board_c_api.h"

#define TAG "XX+EchoBase"

#define PI4IOE_ADDR          0x43
#define PI4IOE_REG_IO_PP     0x07
#define PI4IOE_REG_IO_DIR    0x03
#define PI4IOE_REG_IO_OUT    0x05
#define PI4IOE_REG_IO_PULLUP 0x0D

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    i2c_device_t *pi4ioe;

    led_t *led;
    audio_codec_t *codec;

    board_btn_t *face_button;
} atommatrix_echo_base_ctx_t;

/* Declare circular_strip_led_create (from led_cpp_bridge) */
led_t *circular_strip_led_create(int gpio, int max_leds);

static void on_face_click(void *ud)
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

static void init_i2c(atommatrix_echo_base_ctx_t *ctx)
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

static void i2c_detect(atommatrix_echo_base_ctx_t *ctx)
{
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
            } else if (ret == ESP_ERR_TIMEOUT) {
                printf("UU ");
            } else {
                printf("-- ");
            }
        }
        printf("\r\n");
    }
}

static void init_pi4ioe(atommatrix_echo_base_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init PI4IOE");
    ctx->pi4ioe = i2c_device_create(ctx->i2c_bus, PI4IOE_ADDR);
    i2c_device_write_reg(ctx->pi4ioe, PI4IOE_REG_IO_PP, 0x00);
    i2c_device_write_reg(ctx->pi4ioe, PI4IOE_REG_IO_PULLUP, 0xFF);
    i2c_device_write_reg(ctx->pi4ioe, PI4IOE_REG_IO_DIR, 0x6E);
    i2c_device_write_reg(ctx->pi4ioe, PI4IOE_REG_IO_OUT, 0xFF);
}

static void init_buttons(atommatrix_echo_base_ctx_t *ctx)
{
    board_btn_gpio_cfg_t face_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->face_button = board_btn_create_gpio(&face_cfg);
    board_btn_on_click(ctx->face_button, on_face_click, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_led(board_desc_t *self)
{
    atommatrix_echo_base_ctx_t *ctx = (atommatrix_echo_base_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = circular_strip_led_create(BUILTIN_LED_GPIO, 25);
    }
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    atommatrix_echo_base_ctx_t *ctx = (atommatrix_echo_base_ctx_t *)self;
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

static void destroy(board_desc_t *self)
{
    atommatrix_echo_base_ctx_t *ctx = (atommatrix_echo_base_ctx_t *)self;
    board_btn_delete(ctx->face_button);
    if (ctx->pi4ioe) i2c_device_destroy(ctx->pi4ioe);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    atommatrix_echo_base_ctx_t *ctx = calloc(1, sizeof(atommatrix_echo_base_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.destroy = destroy;

    init_i2c(ctx);
    i2c_detect(ctx);
    init_pi4ioe(ctx);
    init_buttons(ctx);

    return &ctx->base;
}
