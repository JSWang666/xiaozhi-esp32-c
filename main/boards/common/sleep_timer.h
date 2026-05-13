#pragma once

#include <stdbool.h>

#include <esp_timer.h>
#include <esp_pm.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── C API ─────────────────────────────────────────────────────────── */

typedef struct sleep_timer sleep_timer_t;

typedef void (*sleep_timer_callback_fn)(void *user_data);

typedef struct {
    int seconds_to_light_sleep;
    int seconds_to_deep_sleep;
} sleep_timer_cfg_t;

sleep_timer_t *sleep_timer_create(const sleep_timer_cfg_t *cfg);
void sleep_timer_destroy(sleep_timer_t *st);

void sleep_timer_set_enabled(sleep_timer_t *st, bool enabled);
void sleep_timer_on_enter_light_sleep(sleep_timer_t *st, sleep_timer_callback_fn cb, void *user_data);
void sleep_timer_on_exit_light_sleep(sleep_timer_t *st, sleep_timer_callback_fn cb, void *user_data);
void sleep_timer_on_enter_deep_sleep(sleep_timer_t *st, sleep_timer_callback_fn cb, void *user_data);
void sleep_timer_wake_up(sleep_timer_t *st);

#ifdef __cplusplus
}
#endif

/* ── C++ wrapper ───────────────────────────────────────────────────── */
#ifdef __cplusplus
#include <functional>

class SleepTimer {
public:
    SleepTimer(int seconds_to_light_sleep = 20, int seconds_to_deep_sleep = -1) {
        sleep_timer_cfg_t cfg = {
            .seconds_to_light_sleep = seconds_to_light_sleep,
            .seconds_to_deep_sleep = seconds_to_deep_sleep,
        };
        handle_ = sleep_timer_create(&cfg);
    }
    ~SleepTimer() { if (handle_) sleep_timer_destroy(handle_); }

    void SetEnabled(bool enabled) { sleep_timer_set_enabled(handle_, enabled); }
    void OnEnterLightSleepMode(std::function<void()> callback) {
        on_enter_light_ = std::move(callback);
        sleep_timer_on_enter_light_sleep(handle_, [](void *ud) {
            static_cast<SleepTimer*>(ud)->on_enter_light_();
        }, this);
    }
    void OnExitLightSleepMode(std::function<void()> callback) {
        on_exit_light_ = std::move(callback);
        sleep_timer_on_exit_light_sleep(handle_, [](void *ud) {
            static_cast<SleepTimer*>(ud)->on_exit_light_();
        }, this);
    }
    void OnEnterDeepSleepMode(std::function<void()> callback) {
        on_enter_deep_ = std::move(callback);
        sleep_timer_on_enter_deep_sleep(handle_, [](void *ud) {
            static_cast<SleepTimer*>(ud)->on_enter_deep_();
        }, this);
    }
    void WakeUp() { sleep_timer_wake_up(handle_); }

private:
    sleep_timer_t *handle_ = nullptr;
    std::function<void()> on_enter_light_;
    std::function<void()> on_exit_light_;
    std::function<void()> on_enter_deep_;
};

#endif /* __cplusplus */
