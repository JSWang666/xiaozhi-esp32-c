#pragma once

#include <stdbool.h>

#include <esp_timer.h>
#include <esp_pm.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── C API ─────────────────────────────────────────────────────────── */

typedef struct power_save_timer power_save_timer_t;

typedef void (*power_save_callback_fn)(void *user_data);

typedef struct {
    int cpu_max_freq;
    int seconds_to_sleep;
    int seconds_to_shutdown;
} power_save_timer_cfg_t;

power_save_timer_t *power_save_timer_create(const power_save_timer_cfg_t *cfg);
void power_save_timer_destroy(power_save_timer_t *pst);

void power_save_timer_set_enabled(power_save_timer_t *pst, bool enabled);
void power_save_timer_on_enter_sleep(power_save_timer_t *pst, power_save_callback_fn cb, void *user_data);
void power_save_timer_on_exit_sleep(power_save_timer_t *pst, power_save_callback_fn cb, void *user_data);
void power_save_timer_on_shutdown(power_save_timer_t *pst, power_save_callback_fn cb, void *user_data);
void power_save_timer_wake_up(power_save_timer_t *pst);

#ifdef __cplusplus
}
#endif

/* ── C++ wrapper ───────────────────────────────────────────────────── */
#ifdef __cplusplus
#include <functional>

class PowerSaveTimer {
public:
    PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep = 20, int seconds_to_shutdown = -1) {
        power_save_timer_cfg_t cfg = {
            .cpu_max_freq = cpu_max_freq,
            .seconds_to_sleep = seconds_to_sleep,
            .seconds_to_shutdown = seconds_to_shutdown,
        };
        handle_ = power_save_timer_create(&cfg);
    }
    ~PowerSaveTimer() { if (handle_) power_save_timer_destroy(handle_); }

    void SetEnabled(bool enabled) { power_save_timer_set_enabled(handle_, enabled); }
    void OnEnterSleepMode(std::function<void()> callback) {
        on_enter_ = std::move(callback);
        power_save_timer_on_enter_sleep(handle_, [](void *ud) {
            static_cast<PowerSaveTimer*>(ud)->on_enter_();
        }, this);
    }
    void OnExitSleepMode(std::function<void()> callback) {
        on_exit_ = std::move(callback);
        power_save_timer_on_exit_sleep(handle_, [](void *ud) {
            static_cast<PowerSaveTimer*>(ud)->on_exit_();
        }, this);
    }
    void OnShutdownRequest(std::function<void()> callback) {
        on_shutdown_ = std::move(callback);
        power_save_timer_on_shutdown(handle_, [](void *ud) {
            static_cast<PowerSaveTimer*>(ud)->on_shutdown_();
        }, this);
    }
    void WakeUp() { power_save_timer_wake_up(handle_); }

private:
    power_save_timer_t *handle_ = nullptr;
    std::function<void()> on_enter_;
    std::function<void()> on_exit_;
    std::function<void()> on_shutdown_;
};

#endif /* __cplusplus */
