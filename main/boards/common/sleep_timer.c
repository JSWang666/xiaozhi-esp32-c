#include "sleep_timer.h"
#include "settings.h"
#include "display/display.h"
#include "c_api/app_c_api.h"
#include "c_api/audio_c_api.h"
#include "c_api/board_c_api.h"

#include <stdlib.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "SleepTimer"

struct sleep_timer {
    esp_timer_handle_t timer;
    bool enabled;
    int ticks;
    int seconds_to_light_sleep;
    int seconds_to_deep_sleep;
    bool in_light_sleep;

    sleep_timer_callback_fn on_enter_light;
    void *on_enter_light_ud;
    sleep_timer_callback_fn on_exit_light;
    void *on_exit_light_ud;
    sleep_timer_callback_fn on_enter_deep;
    void *on_enter_deep_ud;
};

typedef struct {
    sleep_timer_t *st;
} sleep_loop_ctx_t;

static void sleep_loop_task(void *arg)
{
    sleep_loop_ctx_t *ctx = (sleep_loop_ctx_t *)arg;
    sleep_timer_t *st = ctx->st;

    while (st->in_light_sleep) {
        board_handle_t *board = board_get_instance();
        display_t *disp = (display_t *)board_get_display(board);
        if (disp) {
            display_update_status_bar(disp, true);
        }
        lv_refr_now(NULL);
        lvgl_port_stop();

        esp_sleep_enable_timer_wakeup(30 * 1000000);
        esp_light_sleep_start();
        lvgl_port_resume();

        int reason = esp_sleep_get_wakeup_cause();
        ESP_LOGI(TAG, "Wake up from light sleep, wakeup_reason: %d", reason);
        if (reason != ESP_SLEEP_WAKEUP_TIMER) {
            break;
        }
    }

    sleep_timer_wake_up(st);
    free(ctx);
}

static void st_check(void *arg)
{
    sleep_timer_t *st = (sleep_timer_t *)arg;

    app_context_t *app = app_get_context();
    if (!app_can_enter_sleep_mode(app)) {
        st->ticks = 0;
        return;
    }

    st->ticks++;
    if (st->seconds_to_light_sleep != -1 && st->ticks >= st->seconds_to_light_sleep) {
        if (!st->in_light_sleep) {
            st->in_light_sleep = true;
            if (st->on_enter_light) {
                st->on_enter_light(st->on_enter_light_ud);
            }

            audio_service_t *audio = audio_service_get_instance();
            bool was_wake_word = false;
            if (audio) {
                was_wake_word = audio_service_is_wake_word_running(audio);
                if (was_wake_word) {
                    audio_service_enable_wake_word(audio, false);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }

            sleep_loop_ctx_t *ctx = (sleep_loop_ctx_t *)malloc(sizeof(sleep_loop_ctx_t));
            if (ctx) {
                ctx->st = st;
                app_schedule(app, sleep_loop_task, ctx);
            }

            if (was_wake_word && audio) {
                audio_service_enable_wake_word(audio, true);
            }
        }
    }

    if (st->seconds_to_deep_sleep != -1 && st->ticks >= st->seconds_to_deep_sleep) {
        if (st->on_enter_deep) {
            st->on_enter_deep(st->on_enter_deep_ud);
        }
        esp_deep_sleep_start();
    }
}

sleep_timer_t *sleep_timer_create(const sleep_timer_cfg_t *cfg)
{
    if (!cfg) return NULL;
    sleep_timer_t *st = calloc(1, sizeof(*st));
    if (!st) return NULL;

    st->seconds_to_light_sleep = cfg->seconds_to_light_sleep;
    st->seconds_to_deep_sleep = cfg->seconds_to_deep_sleep;

    esp_timer_create_args_t timer_args = {
        .callback = st_check,
        .arg = st,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sleep_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &st->timer));
    return st;
}

void sleep_timer_destroy(sleep_timer_t *st)
{
    if (!st) return;
    esp_timer_stop(st->timer);
    esp_timer_delete(st->timer);
    free(st);
}

void sleep_timer_set_enabled(sleep_timer_t *st, bool enabled)
{
    if (!st) return;
    if (enabled && !st->enabled) {
        settings_t *settings = settings_open("wifi", false);
        if (settings) {
            bool sleep_mode = settings_get_bool(settings, "sleep_mode", true);
            settings_close(settings);
            if (!sleep_mode) {
                ESP_LOGI(TAG, "Sleep timer is disabled by settings");
                return;
            }
        }
        st->ticks = 0;
        st->enabled = true;
        ESP_ERROR_CHECK(esp_timer_start_periodic(st->timer, 1000000));
        ESP_LOGI(TAG, "Sleep timer enabled");
    } else if (!enabled && st->enabled) {
        ESP_ERROR_CHECK(esp_timer_stop(st->timer));
        st->enabled = false;
        sleep_timer_wake_up(st);
        ESP_LOGI(TAG, "Sleep timer disabled");
    }
}

void sleep_timer_on_enter_light_sleep(sleep_timer_t *st, sleep_timer_callback_fn cb, void *user_data)
{
    if (!st) return;
    st->on_enter_light = cb;
    st->on_enter_light_ud = user_data;
}

void sleep_timer_on_exit_light_sleep(sleep_timer_t *st, sleep_timer_callback_fn cb, void *user_data)
{
    if (!st) return;
    st->on_exit_light = cb;
    st->on_exit_light_ud = user_data;
}

void sleep_timer_on_enter_deep_sleep(sleep_timer_t *st, sleep_timer_callback_fn cb, void *user_data)
{
    if (!st) return;
    st->on_enter_deep = cb;
    st->on_enter_deep_ud = user_data;
}

void sleep_timer_wake_up(sleep_timer_t *st)
{
    if (!st) return;
    st->ticks = 0;
    if (st->in_light_sleep) {
        st->in_light_sleep = false;
        if (st->on_exit_light) {
            st->on_exit_light(st->on_exit_light_ud);
        }
    }
}
