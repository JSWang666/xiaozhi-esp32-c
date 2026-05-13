#ifndef _SYSTEM_RESET_H
#define _SYSTEM_RESET_H

#include <driver/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct system_reset system_reset_t;

system_reset_t *system_reset_create(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin);
void system_reset_destroy(system_reset_t *sr);
void system_reset_check_buttons(system_reset_t *sr);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
class SystemReset {
public:
    SystemReset(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin)
        : handle_(system_reset_create(reset_nvs_pin, reset_factory_pin)) {}
    ~SystemReset() { if (handle_) system_reset_destroy(handle_); }

    void CheckButtons() { if (handle_) system_reset_check_buttons(handle_); }

private:
    SystemReset(const SystemReset&) = delete;
    SystemReset& operator=(const SystemReset&) = delete;
    system_reset_t *handle_;
};
#endif

#endif
