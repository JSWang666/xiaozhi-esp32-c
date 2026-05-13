#include "single_led.h"
#include "application.h"

SingleLed::SingleLed(gpio_num_t gpio)
    : Led(single_led_create(gpio)) {}

void SingleLed::OnStateChanged() {
    if (c_led_) {
        auto& app = Application::GetInstance();
        c_led_->ops->on_state_changed(c_led_, app.GetDeviceState(), app.IsVoiceDetected());
    }
}
