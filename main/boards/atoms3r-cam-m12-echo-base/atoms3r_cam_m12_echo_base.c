#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "boards/common/i2c_device.h"
#include "device_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_camera.h>
#include "c_api/board_c_api.h"

#define TAG "AtomS3R CAM/M12 + EchoBase"

#define PI4IOE_ADDR          0x43
#define PI4IOE_REG_IO_PP     0x07
#define PI4IOE_REG_IO_DIR    0x03
#define PI4IOE_REG_IO_OUT    0x05
#define PI4IOE_REG_IO_PULLUP 0x0D

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    i2c_device_t *pi4ioe;

    audio_codec_t *codec;

    bool is_echo_base_connected;
} atoms3r_cam_ctx_t;

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

static void enable_camera_power(void)
{
    gpio_reset_pin((gpio_num_t)18);
    gpio_set_direction((gpio_num_t)18, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode((gpio_num_t)18, GPIO_PULLDOWN_ONLY);

    ESP_LOGI(TAG, "Camera Power Enabled");
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void init_camera(void)
{
    camera_config_t config = {0};
    config.pin_d0 = CAMERA_PIN_D0;
    config.pin_d1 = CAMERA_PIN_D1;
    config.pin_d2 = CAMERA_PIN_D2;
    config.pin_d3 = CAMERA_PIN_D3;
    config.pin_d4 = CAMERA_PIN_D4;
    config.pin_d5 = CAMERA_PIN_D5;
    config.pin_d6 = CAMERA_PIN_D6;
    config.pin_d7 = CAMERA_PIN_D7;
    config.pin_xclk = CAMERA_PIN_XCLK;
    config.pin_pclk = CAMERA_PIN_PCLK;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;
    config.pin_sccb_sda = CAMERA_PIN_SIOD;
    config.pin_sccb_scl = CAMERA_PIN_SIOC;
    config.sccb_i2c_port = 1;
    config.pin_pwdn = CAMERA_PIN_PWDN;
    config.pin_reset = CAMERA_PIN_RESET;
    config.xclk_freq_hz = XCLK_FREQ_HZ;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_hmirror(s, 0);
    }
}

static void init_i2c(atoms3r_cam_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
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

static void i2c_detect(atoms3r_cam_ctx_t *ctx)
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

static void check_echo_base_connection(atoms3r_cam_ctx_t *ctx)
{
    if (ctx->is_echo_base_connected) {
        return;
    }

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

static void init_pi4ioe(atoms3r_cam_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Init PI4IOE");
    ctx->pi4ioe = i2c_device_create(ctx->i2c_bus, PI4IOE_ADDR);
    i2c_device_write_reg(ctx->pi4ioe, PI4IOE_REG_IO_PP, 0x00);
    i2c_device_write_reg(ctx->pi4ioe, PI4IOE_REG_IO_PULLUP, 0xFF);
    i2c_device_write_reg(ctx->pi4ioe, PI4IOE_REG_IO_DIR, 0x6E);
    i2c_device_write_reg(ctx->pi4ioe, PI4IOE_REG_IO_OUT, 0xFF);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_audio_codec(board_desc_t *self)
{
    atoms3r_cam_ctx_t *ctx = (atoms3r_cam_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(
            ctx->i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_GPIO_PA, AUDIO_CODEC_ES8311_ADDR, false, false);
    }
    return ctx->codec;
}

static void destroy(board_desc_t *self)
{
    atoms3r_cam_ctx_t *ctx = (atoms3r_cam_ctx_t *)self;
    if (ctx->pi4ioe) i2c_device_destroy(ctx->pi4ioe);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    atoms3r_cam_ctx_t *ctx = calloc(1, sizeof(atoms3r_cam_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.destroy = destroy;

    enable_camera_power();
    init_camera();
    init_i2c(ctx);
    i2c_detect(ctx);
    check_echo_base_connection(ctx);
    init_pi4ioe(ctx);

    return &ctx->base;
}
