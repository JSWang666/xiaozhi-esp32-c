#include "app_runtime.h"

#include <stddef.h>

static EventBits_t to_event_bits(app_event_t event) {
    switch (event) {
        case APP_EVENT_SCHEDULE:
            return MAIN_EVENT_SCHEDULE;
        case APP_EVENT_SEND_AUDIO:
            return MAIN_EVENT_SEND_AUDIO;
        case APP_EVENT_WAKE_WORD_DETECTED:
            return MAIN_EVENT_WAKE_WORD_DETECTED;
        case APP_EVENT_VAD_CHANGE:
            return MAIN_EVENT_VAD_CHANGE;
        case APP_EVENT_ERROR:
            return MAIN_EVENT_ERROR;
        case APP_EVENT_ACTIVATION_DONE:
            return MAIN_EVENT_ACTIVATION_DONE;
        case APP_EVENT_CLOCK_TICK:
            return MAIN_EVENT_CLOCK_TICK;
        case APP_EVENT_NETWORK_CONNECTED:
            return MAIN_EVENT_NETWORK_CONNECTED;
        case APP_EVENT_NETWORK_DISCONNECTED:
            return MAIN_EVENT_NETWORK_DISCONNECTED;
        case APP_EVENT_TOGGLE_CHAT:
            return MAIN_EVENT_TOGGLE_CHAT;
        case APP_EVENT_START_LISTENING:
            return MAIN_EVENT_START_LISTENING;
        case APP_EVENT_STOP_LISTENING:
            return MAIN_EVENT_STOP_LISTENING;
        case APP_EVENT_STATE_CHANGED:
            return MAIN_EVENT_STATE_CHANGED;
        default:
            return 0;
    }
}

esp_err_t app_runtime_init(app_runtime_t *rt) {
    if (rt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rt->event_group = xEventGroupCreate();
    if (rt->event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }
    rt->initialized = true;
    return ESP_OK;
}

void app_runtime_deinit(app_runtime_t *rt) {
    if (rt == NULL || rt->event_group == NULL) {
        return;
    }
    vEventGroupDelete(rt->event_group);
    rt->event_group = NULL;
    rt->initialized = false;
}

esp_err_t app_runtime_post_event(app_runtime_t *rt, app_event_t event) {
    EventBits_t bits;

    if (rt == NULL || rt->event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    bits = to_event_bits(event);
    if (bits == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xEventGroupSetBits(rt->event_group, bits);
    return ESP_OK;
}

EventBits_t app_runtime_wait_events(app_runtime_t *rt, EventBits_t mask,
                                    TickType_t wait_ticks) {
    if (rt == NULL || rt->event_group == NULL) {
        return 0;
    }
    return xEventGroupWaitBits(rt->event_group, mask, pdTRUE, pdFALSE,
                               wait_ticks);
}
