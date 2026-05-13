#include "backlight.h"
#include "settings.h"

#include <stdlib.h>
#include <esp_log.h>
#include <driver/ledc.h>

#define TAG "Backlight"

struct backlight {
    backlight_impl_t impl;
    esp_timer_handle_t transition_timer;
    uint8_t brightness;
    uint8_t target_brightness;
    int8_t step;
};

static void transition_timer_cb(void *arg)
{
    backlight_t *bl = (backlight_t *)arg;

    if (bl->brightness == bl->target_brightness) {
        esp_timer_stop(bl->transition_timer);
        return;
    }

    bl->brightness += bl->step;
    if (bl->impl.set_brightness) {
        bl->impl.set_brightness(bl->impl.impl_ctx, bl->brightness);
    }

    if (bl->brightness == bl->target_brightness) {
        esp_timer_stop(bl->transition_timer);
    }
}

backlight_t *backlight_create(const backlight_impl_t *impl)
{
    backlight_t *bl = calloc(1, sizeof(*bl));
    if (!bl) return NULL;

    if (impl) {
        bl->impl = *impl;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = transition_timer_cb,
        .arg = bl,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "backlight_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &bl->transition_timer));
    return bl;
}

void backlight_destroy(backlight_t *bl)
{
    if (!bl) return;
    if (bl->transition_timer) {
        esp_timer_stop(bl->transition_timer);
        esp_timer_delete(bl->transition_timer);
    }
    if (bl->impl.destroy) {
        bl->impl.destroy(bl->impl.impl_ctx);
    }
    free(bl);
}

void backlight_restore_brightness(backlight_t *bl)
{
    if (!bl) return;

    settings_t *s = settings_open("display", false);
    int32_t saved = settings_get_int(s, "brightness", 75);
    settings_close(s);

    if (saved <= 0) {
        ESP_LOGW(TAG, "Brightness value (%d) is too small, setting to default (10)", (int)saved);
        saved = 10;
    }

    backlight_set_brightness(bl, (uint8_t)saved, false);
}

void backlight_set_brightness(backlight_t *bl, uint8_t brightness, bool permanent)
{
    if (!bl) return;
    if (brightness > 100) brightness = 100;
    if (bl->brightness == brightness) return;

    if (permanent) {
        settings_t *s = settings_open("display", true);
        settings_set_int(s, "brightness", brightness);
        settings_close(s);
    }

    bl->target_brightness = brightness;
    bl->step = (bl->target_brightness > bl->brightness) ? 1 : -1;

    if (bl->transition_timer) {
        esp_timer_start_periodic(bl->transition_timer, 5 * 1000);
    }
    ESP_LOGI(TAG, "Set brightness to %d", brightness);
}

uint8_t backlight_get_brightness(const backlight_t *bl)
{
    return bl ? bl->brightness : 0;
}

/* ── PWM backlight ─────────────────────────────────────────────────── */

typedef struct {
    gpio_num_t pin;
} pwm_ctx_t;

static void pwm_set_brightness(void *ctx, uint8_t brightness)
{
    uint32_t duty_cycle = (1023 * brightness) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void pwm_destroy(void *ctx)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    free(ctx);
}

backlight_t *pwm_backlight_create(gpio_num_t pin, bool output_invert, uint32_t freq_hz)
{
    pwm_ctx_t *pctx = calloc(1, sizeof(*pctx));
    if (!pctx) return NULL;
    pctx->pin = pin;

    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));

    const ledc_channel_config_t backlight_channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = output_invert ? 1U : 0U,
        },
    };
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));

    backlight_impl_t impl = {
        .set_brightness = pwm_set_brightness,
        .destroy = pwm_destroy,
        .impl_ctx = pctx,
    };
    return backlight_create(&impl);
}
