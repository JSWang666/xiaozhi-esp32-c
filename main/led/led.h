#ifndef _LED_H_
#define _LED_H_

#include "device_state.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct led_t led_t;

typedef struct led_ops {
    void (*on_state_changed)(led_t *led, DeviceState state, bool is_voice_detected);
    void (*destroy)(led_t *led);
} led_ops_t;

struct led_t {
    const led_ops_t *ops;
};

static inline void led_on_state_changed(led_t *led, DeviceState state, bool voice) {
    if (led && led->ops && led->ops->on_state_changed)
        led->ops->on_state_changed(led, state, voice);
}

static inline void led_destroy(led_t *led) {
    if (led && led->ops && led->ops->destroy)
        led->ops->destroy(led);
}

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class Led {
public:
    Led() : c_led_(nullptr) {}
    explicit Led(led_t *c_led) : c_led_(c_led) {}
    virtual ~Led() {
        if (c_led_) {
            led_destroy(c_led_);
            c_led_ = nullptr;
        }
    }
    virtual void OnStateChanged() = 0;
    led_t *c_led() { return c_led_; }
protected:
    led_t *c_led_;
};

class NoLed : public Led {
public:
    void OnStateChanged() override {}
};

#endif

#endif /* _LED_H_ */
