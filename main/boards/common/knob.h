#ifndef KNOB_H_
#define KNOB_H_

#include <stdbool.h>
#include <stdint.h>

#include <driver/gpio.h>
#include <iot_knob.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct board_knob board_knob_t;

typedef void (*board_knob_rotate_fn)(bool clockwise, void *user_data);

board_knob_t *board_knob_create(gpio_num_t pin_a, gpio_num_t pin_b);
void board_knob_delete(board_knob_t *knob);
void board_knob_on_rotate(board_knob_t *knob, board_knob_rotate_fn cb, void *user_data);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <functional>

class Knob {
public:
    Knob(gpio_num_t pin_a, gpio_num_t pin_b)
        : handle_(board_knob_create(pin_a, pin_b)) {}
    ~Knob() { if (handle_) board_knob_delete(handle_); }

    void OnRotate(std::function<void(bool)> callback) {
        on_rotate_ = std::move(callback);
        board_knob_on_rotate(handle_, invoke_rotate, this);
    }

private:
    Knob(const Knob&) = delete;
    Knob& operator=(const Knob&) = delete;

    static void invoke_rotate(bool clockwise, void *user_data) {
        auto *self = static_cast<Knob*>(user_data);
        if (self->on_rotate_) self->on_rotate_(clockwise);
    }

    board_knob_t *handle_;
    std::function<void(bool)> on_rotate_;
};

#endif /* __cplusplus */

#endif /* KNOB_H_ */
