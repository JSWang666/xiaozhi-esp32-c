#ifndef APPLICATION_C_ACCESS_H
#define APPLICATION_C_ACCESS_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * After `Application` is constructed (first `app_init`), returns the same
 * EventGroupHandle used by `Application::Run()`. Bit layout: `main_event_bits.h`.
 */
EventGroupHandle_t application_main_event_group(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_C_ACCESS_H */
