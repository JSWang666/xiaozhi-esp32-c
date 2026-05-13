#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <driver/gpio.h>
#include <esp_timer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── C API ─────────────────────────────────────────────────────────── */

typedef struct backlight backlight_t;

typedef void (*backlight_set_brightness_fn)(void *impl_ctx, uint8_t brightness);
typedef void (*backlight_destroy_impl_fn)(void *impl_ctx);

typedef struct {
    backlight_set_brightness_fn set_brightness;
    backlight_destroy_impl_fn destroy;
    void *impl_ctx;
} backlight_impl_t;

backlight_t *backlight_create(const backlight_impl_t *impl);
void backlight_destroy(backlight_t *bl);

void backlight_restore_brightness(backlight_t *bl);
void backlight_set_brightness(backlight_t *bl, uint8_t brightness, bool permanent);
uint8_t backlight_get_brightness(const backlight_t *bl);

backlight_t *pwm_backlight_create(gpio_num_t pin, bool output_invert, uint32_t freq_hz);

#ifdef __cplusplus
}
#endif

/* ── C++ wrapper classes (delegate to C API) ──────────────────────── */
#ifdef __cplusplus

class Backlight {
public:
    explicit Backlight(backlight_t *handle) : handle_(handle) {}
    virtual ~Backlight() {
        if (handle_) { backlight_destroy(handle_); handle_ = nullptr; }
    }
    void RestoreBrightness() { backlight_restore_brightness(handle_); }
    void SetBrightness(uint8_t brightness, bool permanent = false) {
        backlight_set_brightness(handle_, brightness, permanent);
    }
    uint8_t brightness() const { return backlight_get_brightness(handle_); }
    backlight_t *c_handle() { return handle_; }

protected:
    Backlight() : handle_(nullptr) {}
    backlight_t *handle_;
};

class PwmBacklight : public Backlight {
public:
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000)
        : Backlight(pwm_backlight_create(pin, output_invert, freq_hz)) {}
};

#endif /* __cplusplus */
