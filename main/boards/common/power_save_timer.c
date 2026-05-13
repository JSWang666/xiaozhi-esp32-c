#include "power_save_timer.h"
#include "settings.h"
#include "c_api/app_c_api.h"
#include "c_api/audio_c_api.h"
#include "c_api/board_c_api.h"
#include "audio_codec.h"

#include <stdlib.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "PowerSaveTimer"

typedef struct {
    power_save_callback_fn fn;
    void *user_data;
} pst_cb_entry_t;

struct power_save_timer {
    esp_timer_handle_t timer;
    bool enabled;
    bool in_sleep_mode;
    bool was_wake_word_running;
    int ticks;
    int cpu_max_freq;
    int seconds_to_sleep;
    int seconds_to_shutdown;

    pst_cb_entry_t on_enter_sleep;
    pst_cb_entry_t on_exit_sleep;
    pst_cb_entry_t on_shutdown;
};

static void pst_check(void *arg)
{
    power_save_timer_t *pst = (power_save_timer_t *)arg;

    app_context_t *app = app_get_context();
    if (!pst->in_sleep_mode && !app_can_enter_sleep_mode(app)) {
        pst->ticks = 0;
        return;
    }

    pst->ticks++;
    if (pst->seconds_to_sleep != -1 && pst->ticks >= pst->seconds_to_sleep) {
        if (!pst->in_sleep_mode) {
            ESP_LOGI(TAG, "Enabling power save mode");
            pst->in_sleep_mode = true;
            if (pst->on_enter_sleep.fn) {
                pst->on_enter_sleep.fn(pst->on_enter_sleep.user_data);
            }

            if (pst->cpu_max_freq != -1) {
                audio_service_t *audio = audio_service_get_instance();
                if (audio) {
                    pst->was_wake_word_running = audio_service_is_wake_word_running(audio);
                    if (pst->was_wake_word_running) {
                        audio_service_enable_wake_word(audio, false);
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                }

                board_handle_t *board = board_get_instance();
                audio_codec_t *codec = (audio_codec_t *)board_get_audio_codec(board);
                if (codec && codec->ops && codec->ops->enable_input) {
                    codec->ops->enable_input(codec, false);
                }

                esp_pm_config_t pm_config = {
                    .max_freq_mhz = pst->cpu_max_freq,
                    .min_freq_mhz = 40,
                    .light_sleep_enable = true,
                };
                esp_pm_configure(&pm_config);
            }
        }
    }
    if (pst->seconds_to_shutdown != -1 && pst->ticks >= pst->seconds_to_shutdown
        && pst->on_shutdown.fn) {
        pst->on_shutdown.fn(pst->on_shutdown.user_data);
    }
}

power_save_timer_t *power_save_timer_create(const power_save_timer_cfg_t *cfg)
{
    if (!cfg) return NULL;
    power_save_timer_t *pst = calloc(1, sizeof(*pst));
    if (!pst) return NULL;

    pst->cpu_max_freq = cfg->cpu_max_freq;
    pst->seconds_to_sleep = cfg->seconds_to_sleep;
    pst->seconds_to_shutdown = cfg->seconds_to_shutdown;

    esp_timer_create_args_t timer_args = {
        .callback = pst_check,
        .arg = pst,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "power_save_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &pst->timer));
    return pst;
}

void power_save_timer_destroy(power_save_timer_t *pst)
{
    if (!pst) return;
    esp_timer_stop(pst->timer);
    esp_timer_delete(pst->timer);
    free(pst);
}

void power_save_timer_set_enabled(power_save_timer_t *pst, bool enabled)
{
    if (!pst) return;
    if (enabled && !pst->enabled) {
        settings_t *settings = settings_open("wifi", false);
        if (settings) {
            bool sleep_mode = settings_get_bool(settings, "sleep_mode", true);
            settings_close(settings);
            if (!sleep_mode) {
                ESP_LOGI(TAG, "Power save timer is disabled by settings");
                return;
            }
        }
        pst->ticks = 0;
        pst->enabled = true;
        ESP_ERROR_CHECK(esp_timer_start_periodic(pst->timer, 1000000));
        ESP_LOGI(TAG, "Power save timer enabled");
    } else if (!enabled && pst->enabled) {
        ESP_ERROR_CHECK(esp_timer_stop(pst->timer));
        pst->enabled = false;
        power_save_timer_wake_up(pst);
        ESP_LOGI(TAG, "Power save timer disabled");
    }
}

void power_save_timer_on_enter_sleep(power_save_timer_t *pst, power_save_callback_fn cb, void *user_data)
{
    if (!pst) return;
    pst->on_enter_sleep.fn = cb;
    pst->on_enter_sleep.user_data = user_data;
}

void power_save_timer_on_exit_sleep(power_save_timer_t *pst, power_save_callback_fn cb, void *user_data)
{
    if (!pst) return;
    pst->on_exit_sleep.fn = cb;
    pst->on_exit_sleep.user_data = user_data;
}

void power_save_timer_on_shutdown(power_save_timer_t *pst, power_save_callback_fn cb, void *user_data)
{
    if (!pst) return;
    pst->on_shutdown.fn = cb;
    pst->on_shutdown.user_data = user_data;
}

void power_save_timer_wake_up(power_save_timer_t *pst)
{
    if (!pst) return;
    pst->ticks = 0;
    if (pst->in_sleep_mode) {
        ESP_LOGI(TAG, "Exiting power save mode");
        pst->in_sleep_mode = false;

        if (pst->cpu_max_freq != -1) {
            esp_pm_config_t pm_config = {
                .max_freq_mhz = pst->cpu_max_freq,
                .min_freq_mhz = pst->cpu_max_freq,
                .light_sleep_enable = false,
            };
            esp_pm_configure(&pm_config);

            if (pst->was_wake_word_running) {
                audio_service_t *audio = audio_service_get_instance();
                if (audio) {
                    audio_service_enable_wake_word(audio, true);
                }
            }
        }

        if (pst->on_exit_sleep.fn) {
            pst->on_exit_sleep.fn(pst->on_exit_sleep.user_data);
        }
    }
}
