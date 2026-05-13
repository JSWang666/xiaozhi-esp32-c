#include "board_defs.h"
#include "button.h"
#include "system_reset.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "audio/codecs/no_audio_codec.h"
#include "display/display.h"
#include "device_state.h"

#include <stdlib.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include "c_api/board_c_api.h"

extern led_t *gpio_led_create(int gpio);

#define TAG "DfrobotEsp32S3AiCam"

typedef struct {
    board_desc_t base;

    led_t *led;
    audio_codec_t *codec;

    board_btn_t *boot_button;
} df_s3_cam_ctx_t;

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

static void init_buttons(df_s3_cam_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static const char *df_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *df_get_led(board_desc_t *self)
{
    df_s3_cam_ctx_t *ctx = (df_s3_cam_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = gpio_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *df_get_audio_codec(board_desc_t *self)
{
    df_s3_cam_ctx_t *ctx = (df_s3_cam_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = no_audio_codec_simplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, GPIO_NUM_NC, AUDIO_I2S_MIC_GPIO_DIN);
    }
    return ctx->codec;
}

static void df_destroy(board_desc_t *self)
{
    df_s3_cam_ctx_t *ctx = (df_s3_cam_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    df_s3_cam_ctx_t *ctx = calloc(1, sizeof(df_s3_cam_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = df_get_board_type;
    ctx->base.get_led = df_get_led;
    ctx->base.get_audio_codec = df_get_audio_codec;
    ctx->base.destroy = df_destroy;

    init_buttons(ctx);

    return &ctx->base;
}
