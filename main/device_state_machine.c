#include "device_state_machine.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *TAG = "StateMachine";

static const char *const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "wifi_configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

typedef struct {
    int id;
    device_state_callback_t callback;
    void *ctx;
} listener_entry_t;

struct device_state_machine {
    DeviceState current_state;
    listener_entry_t listeners[DEVICE_STATE_MACHINE_MAX_LISTENERS];
    int listener_count;
    int next_listener_id;
    SemaphoreHandle_t mutex;
};

static bool is_valid_transition(DeviceState from, DeviceState to)
{
    if (from == to)
        return true;

    switch (from) {
    case kDeviceStateUnknown:
        return to == kDeviceStateStarting;

    case kDeviceStateStarting:
        return to == kDeviceStateWifiConfiguring ||
               to == kDeviceStateActivating;

    case kDeviceStateWifiConfiguring:
        return to == kDeviceStateActivating ||
               to == kDeviceStateAudioTesting;

    case kDeviceStateAudioTesting:
        return to == kDeviceStateWifiConfiguring;

    case kDeviceStateActivating:
        return to == kDeviceStateUpgrading ||
               to == kDeviceStateIdle ||
               to == kDeviceStateWifiConfiguring;

    case kDeviceStateUpgrading:
        return to == kDeviceStateIdle ||
               to == kDeviceStateActivating;

    case kDeviceStateIdle:
        return to == kDeviceStateConnecting ||
               to == kDeviceStateListening ||
               to == kDeviceStateSpeaking ||
               to == kDeviceStateActivating ||
               to == kDeviceStateUpgrading ||
               to == kDeviceStateWifiConfiguring;

    case kDeviceStateConnecting:
        return to == kDeviceStateIdle ||
               to == kDeviceStateListening;

    case kDeviceStateListening:
        return to == kDeviceStateSpeaking ||
               to == kDeviceStateIdle;

    case kDeviceStateSpeaking:
        return to == kDeviceStateListening ||
               to == kDeviceStateIdle;

    case kDeviceStateFatalError:
        return false;

    default:
        return false;
    }
}

static void notify_listeners(device_state_machine_t *sm, DeviceState old_state, DeviceState new_state)
{
    device_state_callback_t callbacks[DEVICE_STATE_MACHINE_MAX_LISTENERS];
    void *contexts[DEVICE_STATE_MACHINE_MAX_LISTENERS];
    int count = 0;

    xSemaphoreTake(sm->mutex, portMAX_DELAY);
    for (int i = 0; i < sm->listener_count; i++) {
        callbacks[count] = sm->listeners[i].callback;
        contexts[count] = sm->listeners[i].ctx;
        count++;
    }
    xSemaphoreGive(sm->mutex);

    for (int i = 0; i < count; i++) {
        callbacks[i](contexts[i], old_state, new_state);
    }
}

device_state_machine_t *device_state_machine_create(void)
{
    device_state_machine_t *sm = (device_state_machine_t *)calloc(1, sizeof(*sm));
    if (!sm) return NULL;

    sm->current_state = kDeviceStateUnknown;
    sm->mutex = xSemaphoreCreateMutex();
    if (!sm->mutex) {
        free(sm);
        return NULL;
    }
    return sm;
}

void device_state_machine_destroy(device_state_machine_t *sm)
{
    if (!sm) return;
    if (sm->mutex) vSemaphoreDelete(sm->mutex);
    free(sm);
}

DeviceState device_state_machine_get_state(const device_state_machine_t *sm)
{
    if (!sm) return kDeviceStateUnknown;
    return sm->current_state;
}

bool device_state_machine_transition_to(device_state_machine_t *sm, DeviceState new_state)
{
    if (!sm) return false;

    DeviceState old_state = sm->current_state;
    if (old_state == new_state)
        return true;

    if (!is_valid_transition(old_state, new_state)) {
        ESP_LOGW(TAG, "Invalid state transition: %s -> %s",
                 device_state_get_name(old_state), device_state_get_name(new_state));
        return false;
    }

    sm->current_state = new_state;
    ESP_LOGI(TAG, "State: %s -> %s",
             device_state_get_name(old_state), device_state_get_name(new_state));

    notify_listeners(sm, old_state, new_state);
    return true;
}

bool device_state_machine_can_transition_to(const device_state_machine_t *sm, DeviceState target)
{
    if (!sm) return false;
    return is_valid_transition(sm->current_state, target);
}

int device_state_machine_add_listener(device_state_machine_t *sm,
                                      device_state_callback_t callback, void *ctx)
{
    if (!sm || !callback) return -1;

    xSemaphoreTake(sm->mutex, portMAX_DELAY);
    if (sm->listener_count >= DEVICE_STATE_MACHINE_MAX_LISTENERS) {
        xSemaphoreGive(sm->mutex);
        ESP_LOGW(TAG, "Max listeners reached");
        return -1;
    }

    int id = sm->next_listener_id++;
    listener_entry_t *entry = &sm->listeners[sm->listener_count++];
    entry->id = id;
    entry->callback = callback;
    entry->ctx = ctx;
    xSemaphoreGive(sm->mutex);
    return id;
}

void device_state_machine_remove_listener(device_state_machine_t *sm, int listener_id)
{
    if (!sm) return;

    xSemaphoreTake(sm->mutex, portMAX_DELAY);
    for (int i = 0; i < sm->listener_count; i++) {
        if (sm->listeners[i].id == listener_id) {
            sm->listeners[i] = sm->listeners[sm->listener_count - 1];
            sm->listener_count--;
            break;
        }
    }
    xSemaphoreGive(sm->mutex);
}

const char *device_state_get_name(DeviceState state)
{
    if (state >= 0 && state <= kDeviceStateFatalError) {
        return STATE_STRINGS[state];
    }
    return STATE_STRINGS[kDeviceStateFatalError + 1];
}
