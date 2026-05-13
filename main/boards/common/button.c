#include "button.h"

#include <stdlib.h>
#include <esp_log.h>

#define TAG "Button"

typedef struct {
    board_btn_callback_fn fn;
    void *user_data;
} btn_cb_entry_t;

struct board_btn {
    button_handle_t iot_handle;
    bool owns_handle;
    btn_cb_entry_t on_press_down;
    btn_cb_entry_t on_press_up;
    btn_cb_entry_t on_long_press;
    btn_cb_entry_t on_click;
    btn_cb_entry_t on_double_click;
    btn_cb_entry_t on_multiple_click;
};

static board_btn_t *alloc_btn(void)
{
    board_btn_t *btn = calloc(1, sizeof(*btn));
    return btn;
}

static void iot_cb_trampoline(void *button_handle, void *usr_data)
{
    btn_cb_entry_t *entry = (btn_cb_entry_t *)usr_data;
    if (entry && entry->fn) {
        entry->fn(entry->user_data);
    }
}

board_btn_t *board_btn_create_gpio(const board_btn_gpio_cfg_t *cfg)
{
    if (!cfg || cfg->gpio_num == GPIO_NUM_NC) return NULL;

    board_btn_t *btn = alloc_btn();
    if (!btn) return NULL;

    button_config_t button_config = {
        .long_press_time = cfg->long_press_time,
        .short_press_time = cfg->short_press_time,
    };
    button_gpio_config_t gpio_config = {
        .gpio_num = cfg->gpio_num,
        .active_level = (uint8_t)(cfg->active_high ? 1 : 0),
        .enable_power_save = cfg->enable_power_save,
        .disable_pull = false,
    };
    esp_err_t err = iot_button_new_gpio_device(&button_config, &gpio_config, &btn->iot_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create GPIO button: %s", esp_err_to_name(err));
        free(btn);
        return NULL;
    }
    btn->owns_handle = true;
    return btn;
}

board_btn_t *board_btn_create_from_handle(button_handle_t handle)
{
    board_btn_t *btn = alloc_btn();
    if (!btn) return NULL;
    btn->iot_handle = handle;
    btn->owns_handle = false;
    return btn;
}

#if CONFIG_SOC_ADC_SUPPORTED
board_btn_t *board_btn_create_adc(const button_adc_config_t *adc_cfg)
{
    if (!adc_cfg) return NULL;

    board_btn_t *btn = alloc_btn();
    if (!btn) return NULL;

    button_config_t btn_config = {
        .long_press_time = 2000,
        .short_press_time = 0,
    };
    esp_err_t err = iot_button_new_adc_device(&btn_config, adc_cfg, &btn->iot_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ADC button: %s", esp_err_to_name(err));
        free(btn);
        return NULL;
    }
    btn->owns_handle = true;
    return btn;
}
#endif

void board_btn_delete(board_btn_t *btn)
{
    if (!btn) return;
    if (btn->iot_handle && btn->owns_handle) {
        iot_button_delete(btn->iot_handle);
    }
    free(btn);
}

void board_btn_on_press_down(board_btn_t *btn, board_btn_callback_fn cb, void *user_data)
{
    if (!btn || !btn->iot_handle) return;
    btn->on_press_down.fn = cb;
    btn->on_press_down.user_data = user_data;
    iot_button_register_cb(btn->iot_handle, BUTTON_PRESS_DOWN, NULL, iot_cb_trampoline, &btn->on_press_down);
}

void board_btn_on_press_up(board_btn_t *btn, board_btn_callback_fn cb, void *user_data)
{
    if (!btn || !btn->iot_handle) return;
    btn->on_press_up.fn = cb;
    btn->on_press_up.user_data = user_data;
    iot_button_register_cb(btn->iot_handle, BUTTON_PRESS_UP, NULL, iot_cb_trampoline, &btn->on_press_up);
}

void board_btn_on_long_press(board_btn_t *btn, board_btn_callback_fn cb, void *user_data)
{
    if (!btn || !btn->iot_handle) return;
    btn->on_long_press.fn = cb;
    btn->on_long_press.user_data = user_data;
    iot_button_register_cb(btn->iot_handle, BUTTON_LONG_PRESS_START, NULL, iot_cb_trampoline, &btn->on_long_press);
}

void board_btn_on_click(board_btn_t *btn, board_btn_callback_fn cb, void *user_data)
{
    if (!btn || !btn->iot_handle) return;
    btn->on_click.fn = cb;
    btn->on_click.user_data = user_data;
    iot_button_register_cb(btn->iot_handle, BUTTON_SINGLE_CLICK, NULL, iot_cb_trampoline, &btn->on_click);
}

void board_btn_on_double_click(board_btn_t *btn, board_btn_callback_fn cb, void *user_data)
{
    if (!btn || !btn->iot_handle) return;
    btn->on_double_click.fn = cb;
    btn->on_double_click.user_data = user_data;
    iot_button_register_cb(btn->iot_handle, BUTTON_DOUBLE_CLICK, NULL, iot_cb_trampoline, &btn->on_double_click);
}

void board_btn_on_multiple_click(board_btn_t *btn, board_btn_callback_fn cb, void *user_data, uint8_t click_count)
{
    if (!btn || !btn->iot_handle) return;
    btn->on_multiple_click.fn = cb;
    btn->on_multiple_click.user_data = user_data;
    button_event_args_t event_args = {
        .multiple_clicks = {
            .clicks = click_count,
        },
    };
    iot_button_register_cb(btn->iot_handle, BUTTON_MULTIPLE_CLICK, &event_args, iot_cb_trampoline, &btn->on_multiple_click);
}
