#ifndef APP_RUNTIME_H
#define APP_RUNTIME_H

#include <stdbool.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <esp_err.h>

#include "app_c_api.h"
#include "main_event_bits.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    EventGroupHandle_t event_group;
    bool initialized;
} app_runtime_t;

esp_err_t app_runtime_init(app_runtime_t *rt);
void app_runtime_deinit(app_runtime_t *rt);
esp_err_t app_runtime_post_event(app_runtime_t *rt, app_event_t event);
EventBits_t app_runtime_wait_events(app_runtime_t *rt, EventBits_t mask, TickType_t wait_ticks);

#ifdef __cplusplus
}
#endif

#endif
