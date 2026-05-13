#ifndef DEVICE_STATE_MACHINE_H
#define DEVICE_STATE_MACHINE_H

#include <stdbool.h>
#include "device_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_STATE_MACHINE_MAX_LISTENERS 8

typedef struct device_state_machine device_state_machine_t;

typedef void (*device_state_callback_t)(void *ctx, DeviceState old_state, DeviceState new_state);

device_state_machine_t *device_state_machine_create(void);
void device_state_machine_destroy(device_state_machine_t *sm);

DeviceState device_state_machine_get_state(const device_state_machine_t *sm);
bool device_state_machine_transition_to(device_state_machine_t *sm, DeviceState new_state);
bool device_state_machine_can_transition_to(const device_state_machine_t *sm, DeviceState target);

int device_state_machine_add_listener(device_state_machine_t *sm,
                                      device_state_callback_t callback, void *ctx);
void device_state_machine_remove_listener(device_state_machine_t *sm, int listener_id);

const char *device_state_get_name(DeviceState state);

#ifdef __cplusplus
}
#endif

/* C++ compatibility wrapper */
#ifdef __cplusplus
#include <functional>

class DeviceStateMachine {
public:
    DeviceStateMachine() : handle_(device_state_machine_create()) {}
    ~DeviceStateMachine() { if (handle_) device_state_machine_destroy(handle_); }

    DeviceStateMachine(const DeviceStateMachine&) = delete;
    DeviceStateMachine& operator=(const DeviceStateMachine&) = delete;

    DeviceState GetState() const {
        return device_state_machine_get_state(handle_);
    }
    bool TransitionTo(DeviceState new_state) {
        return device_state_machine_transition_to(handle_, new_state);
    }
    bool CanTransitionTo(DeviceState target) const {
        return device_state_machine_can_transition_to(handle_, target);
    }

    using StateCallback = std::function<void(DeviceState, DeviceState)>;

    int AddStateChangeListener(StateCallback callback) {
        auto *wrap = new CallbackWrapper{std::move(callback)};
        int id = device_state_machine_add_listener(handle_, &trampoline, wrap);
        for (int i = 0; i < max_wrappers_; i++) {
            if (!wrappers_[i]) { wrappers_[i] = wrap; wrapper_ids_[i] = id; break; }
        }
        return id;
    }

    void RemoveStateChangeListener(int listener_id) {
        device_state_machine_remove_listener(handle_, listener_id);
        for (int i = 0; i < max_wrappers_; i++) {
            if (wrapper_ids_[i] == listener_id && wrappers_[i]) {
                delete wrappers_[i];
                wrappers_[i] = nullptr;
                wrapper_ids_[i] = -1;
                break;
            }
        }
    }

    static const char* GetStateName(DeviceState state) {
        return device_state_get_name(state);
    }

private:
    struct CallbackWrapper { StateCallback fn; };

    static void trampoline(void *ctx, DeviceState old_s, DeviceState new_s) {
        auto *w = static_cast<CallbackWrapper*>(ctx);
        if (w && w->fn) w->fn(old_s, new_s);
    }

    device_state_machine_t *handle_;
    static constexpr int max_wrappers_ = DEVICE_STATE_MACHINE_MAX_LISTENERS;
    CallbackWrapper *wrappers_[DEVICE_STATE_MACHINE_MAX_LISTENERS] = {};
    int wrapper_ids_[DEVICE_STATE_MACHINE_MAX_LISTENERS] = {
        -1, -1, -1, -1, -1, -1, -1, -1
    };
};
#endif

#endif /* DEVICE_STATE_MACHINE_H */
