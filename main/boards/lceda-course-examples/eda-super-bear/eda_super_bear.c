#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/display_c_api.h"
#include "audio/codecs/no_audio_codec.h"
#include "device_state.h"

#include <esp_log.h>
#include <stdlib.h>

#define TAG "EdaSuperBear"

extern void InitializeEdaSuperBearController(void);

typedef struct {
    board_desc_t base;
    display_t *display;
    audio_codec_t *codec;
    board_btn_t *boot_button;
} eda_super_bear_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting)
        return;
    app_toggle_chat(app);
}

static const char *esb_get_board_type(board_desc_t *self)
{
    (void)self;
    return "EdaSuperBear";
}

static void *esb_get_led(board_desc_t *self)
{
    (void)self;
    return NULL;
}

static void *esb_get_audio_codec(board_desc_t *self)
{
    eda_super_bear_ctx_t *ctx = (eda_super_bear_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = no_audio_codec_simplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
    }
    return ctx->codec;
}

static void *esb_get_display(board_desc_t *self)
{
    eda_super_bear_ctx_t *ctx = (eda_super_bear_ctx_t *)self;
    return ctx->display;
}

static void esb_destroy(board_desc_t *self)
{
    eda_super_bear_ctx_t *ctx = (eda_super_bear_ctx_t *)self;
    if (ctx->boot_button)
        board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    eda_super_bear_ctx_t *ctx = calloc(1, sizeof(eda_super_bear_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind            = BOARD_KIND_WIFI;
    ctx->base.get_board_type  = esb_get_board_type;
    ctx->base.get_led         = esb_get_led;
    ctx->base.get_audio_codec = esb_get_audio_codec;
    ctx->base.get_display     = esb_get_display;
    ctx->base.destroy         = esb_destroy;

    ctx->display = no_display_create();
    ESP_LOGI(TAG, "Using NoDisplay (no physical display)");

    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    ESP_LOGI(TAG, "Initializing EdaRobot MCP controller");
    InitializeEdaSuperBearController();

    return &ctx->base;
}
