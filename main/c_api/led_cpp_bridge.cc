#include "led/led.h"
#include "led/circular_strip.h"
#include "led/gpio_led.h"

#include <new>

struct circular_strip_wrapper {
    led_t base;
    CircularStrip *impl;
};

struct gpio_led_wrapper {
    led_t base;
    GpioLed *impl;
};

static void circular_strip_on_state(led_t *led, DeviceState state, bool voice) {
    auto *w = reinterpret_cast<circular_strip_wrapper *>(led);
    if (w && w->impl) w->impl->OnStateChanged(state, voice);
}

static void circular_strip_destroy(led_t *led) {
    auto *w = reinterpret_cast<circular_strip_wrapper *>(led);
    if (w) {
        delete w->impl;
        delete w;
    }
}

static const led_ops_t circular_strip_c_ops = {
    .on_state_changed = circular_strip_on_state,
    .destroy = circular_strip_destroy,
};

static void gpio_led_on_state(led_t *led, DeviceState state, bool voice) {
    auto *w = reinterpret_cast<gpio_led_wrapper *>(led);
    if (w && w->impl) w->impl->OnStateChanged(state, voice);
}

static void gpio_led_destroy(led_t *led) {
    auto *w = reinterpret_cast<gpio_led_wrapper *>(led);
    if (w) {
        delete w->impl;
        delete w;
    }
}

static const led_ops_t gpio_led_c_ops = {
    .on_state_changed = gpio_led_on_state,
    .destroy = gpio_led_destroy,
};

extern "C" led_t *circular_strip_led_create(int gpio, int max_leds) {
    auto *w = new (std::nothrow) circular_strip_wrapper{};
    if (!w) return nullptr;
    w->impl = new (std::nothrow) CircularStrip(static_cast<gpio_num_t>(gpio), max_leds);
    if (!w->impl) { delete w; return nullptr; }
    w->base.ops = &circular_strip_c_ops;
    return &w->base;
}

extern "C" led_t *gpio_led_create(int gpio) {
    auto *w = new (std::nothrow) gpio_led_wrapper{};
    if (!w) return nullptr;
    w->impl = new (std::nothrow) GpioLed(static_cast<gpio_num_t>(gpio));
    if (!w->impl) { delete w; return nullptr; }
    w->base.ops = &gpio_led_c_ops;
    return &w->base;
}

extern "C" led_t *gpio_led_create_ex(int gpio, int output_invert) {
    auto *w = new (std::nothrow) gpio_led_wrapper{};
    if (!w) return nullptr;
    w->impl = new (std::nothrow) GpioLed(static_cast<gpio_num_t>(gpio), output_invert);
    if (!w->impl) { delete w; return nullptr; }
    w->base.ops = &gpio_led_c_ops;
    return &w->base;
}
