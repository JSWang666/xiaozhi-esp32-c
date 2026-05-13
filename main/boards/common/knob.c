#include "knob.h"

#include <stdlib.h>
#include <esp_log.h>

#define TAG "Knob"

typedef struct {
    board_knob_rotate_fn fn;
    void *user_data;
} knob_cb_entry_t;

struct board_knob {
    knob_handle_t iot_handle;
    knob_cb_entry_t on_rotate;
};

static void iot_knob_trampoline(void *knob_handle, void *usr_data)
{
    board_knob_t *knob = (board_knob_t *)usr_data;
    knob_event_t event = iot_knob_get_event(knob_handle);
    if (knob->on_rotate.fn) {
        knob->on_rotate.fn(event == KNOB_RIGHT, knob->on_rotate.user_data);
    }
}

board_knob_t *board_knob_create(gpio_num_t pin_a, gpio_num_t pin_b)
{
    board_knob_t *knob = calloc(1, sizeof(*knob));
    if (!knob) return NULL;

    knob_config_t config = {
        .default_direction = 0,
        .gpio_encoder_a = (uint8_t)pin_a,
        .gpio_encoder_b = (uint8_t)pin_b,
    };

    knob->iot_handle = iot_knob_create(&config);
    if (knob->iot_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create knob instance");
        free(knob);
        return NULL;
    }

    esp_err_t err = iot_knob_register_cb(knob->iot_handle, KNOB_LEFT, iot_knob_trampoline, knob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register left callback: %s", esp_err_to_name(err));
        iot_knob_delete(knob->iot_handle);
        free(knob);
        return NULL;
    }

    err = iot_knob_register_cb(knob->iot_handle, KNOB_RIGHT, iot_knob_trampoline, knob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register right callback: %s", esp_err_to_name(err));
        iot_knob_delete(knob->iot_handle);
        free(knob);
        return NULL;
    }

    ESP_LOGI(TAG, "Knob initialized with pins A:%d B:%d", pin_a, pin_b);
    return knob;
}

void board_knob_delete(board_knob_t *knob)
{
    if (!knob) return;
    if (knob->iot_handle) {
        iot_knob_delete(knob->iot_handle);
    }
    free(knob);
}

void board_knob_on_rotate(board_knob_t *knob, board_knob_rotate_fn cb, void *user_data)
{
    if (!knob) return;
    knob->on_rotate.fn = cb;
    knob->on_rotate.user_data = user_data;
}
