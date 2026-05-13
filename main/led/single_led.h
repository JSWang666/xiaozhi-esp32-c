#ifndef _SINGLE_LED_H_
#define _SINGLE_LED_H_

#include "led.h"
#include <driver/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

led_t *single_led_create(gpio_num_t gpio);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class SingleLed : public Led {
public:
    SingleLed(gpio_num_t gpio);
    void OnStateChanged() override;
};

#endif

#endif /* _SINGLE_LED_H_ */
