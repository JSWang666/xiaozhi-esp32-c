#include "board_defs.h"
#include "button.h"
#include "system_reset.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "led/led.h"
#include "audio/codecs/no_audio_codec.h"
#include "device_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include "c_api/board_c_api.h"

#define TAG "DoitS3AiBox"

/* Declare gpio_led_create (from led_cpp_bridge) */
led_t *gpio_led_create_ex(int gpio, int output_invert);

typedef struct {
    board_desc_t base;

    led_t *led;
    audio_codec_t *codec;

    board_btn_t *boot_button;
    board_btn_t *touch_button;
    board_btn_t *volume_up_button;
    board_btn_t *volume_down_button;

    uint8_t click_times;
    uint32_t check_time;
} doit_s3_ctx_t;

static void on_boot_click(void *ud)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)ud;
    if (ctx->click_times == 0) {
        ctx->check_time = (uint32_t)(esp_timer_get_time() / 1000);
    }
    if ((uint32_t)(esp_timer_get_time() / 1000) - ctx->check_time < 1000) {
        ctx->click_times++;
        ctx->check_time = (uint32_t)(esp_timer_get_time() / 1000);
    } else {
        ctx->click_times = 0;
        ctx->check_time = 0;
    }
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) {
        board_enter_wifi_config_mode(board_get_instance());
        return;
    }
    app_toggle_chat(app);
}

static void on_boot_double_click(void *ud)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)ud;
    ctx->click_times++;
    ESP_LOGI(TAG, "DoubleClick times %d", ctx->click_times);
    if (ctx->click_times == 3) {
        ctx->click_times = 0;
    }
}

static void on_boot_long_press(void *ud)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)ud;
    if (ctx->click_times >= 3) {
        /* reserved for wifi config mode */
    } else {
        ctx->click_times = 0;
        ctx->check_time = 0;
    }
}

static void on_touch_press_down(void *ud)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)ud;
    ctx->click_times = 0;
    app_context_t *app = app_get_context();
    if (app) app_start_listening(app);
}

static void on_touch_press_up(void *ud)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)ud;
    ctx->click_times = 0;
    app_context_t *app = app_get_context();
    if (app) app_stop_listening(app);
}

static void on_volume_up_click(void *ud)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)ud;
    ctx->click_times = 0;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume + 10;
    if (vol > 100) vol = 100;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
}

static void on_volume_up_long(void *ud)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)ud;
    ctx->click_times = 0;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 100);
}

static void on_volume_down_click(void *ud)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)ud;
    ctx->click_times = 0;
    if (!ctx->codec) return;
    int vol = ctx->codec->output_volume - 10;
    if (vol < 0) vol = 0;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, vol);
}

static void on_volume_down_long(void *ud)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)ud;
    ctx->click_times = 0;
    if (!ctx->codec) return;
    if (ctx->codec->ops && ctx->codec->ops->set_output_volume)
        ctx->codec->ops->set_output_volume(ctx->codec, 0);
}

static void init_gpio48(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << GPIO_NUM_48),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_set_level(GPIO_NUM_48, 1);
}

static void init_buttons(doit_s3_ctx_t *ctx)
{
    ctx->click_times = 0;
    ctx->check_time = 0;

    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
    board_btn_on_double_click(ctx->boot_button, on_boot_double_click, ctx);
    board_btn_on_long_press(ctx->boot_button, on_boot_long_press, ctx);

    board_btn_gpio_cfg_t touch_cfg = { .gpio_num = TOUCH_BUTTON_GPIO };
    ctx->touch_button = board_btn_create_gpio(&touch_cfg);
    board_btn_on_press_down(ctx->touch_button, on_touch_press_down, ctx);
    board_btn_on_press_up(ctx->touch_button, on_touch_press_up, ctx);

    board_btn_gpio_cfg_t vol_up_cfg = { .gpio_num = VOLUME_UP_BUTTON_GPIO };
    ctx->volume_up_button = board_btn_create_gpio(&vol_up_cfg);
    board_btn_on_click(ctx->volume_up_button, on_volume_up_click, ctx);
    board_btn_on_long_press(ctx->volume_up_button, on_volume_up_long, ctx);

    board_btn_gpio_cfg_t vol_down_cfg = { .gpio_num = VOLUME_DOWN_BUTTON_GPIO };
    ctx->volume_down_button = board_btn_create_gpio(&vol_down_cfg);
    board_btn_on_click(ctx->volume_down_button, on_volume_down_click, ctx);
    board_btn_on_long_press(ctx->volume_down_button, on_volume_down_long, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_led(board_desc_t *self)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = gpio_led_create_ex(BUILTIN_LED_GPIO, 1);
    }
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = no_audio_codec_simplex_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
    }
    return ctx->codec;
}

static void destroy(board_desc_t *self)
{
    doit_s3_ctx_t *ctx = (doit_s3_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->touch_button);
    board_btn_delete(ctx->volume_up_button);
    board_btn_delete(ctx->volume_down_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    doit_s3_ctx_t *ctx = calloc(1, sizeof(doit_s3_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.destroy = destroy;

    init_gpio48();
    init_buttons(ctx);

    return &ctx->base;
}
