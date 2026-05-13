#ifndef BUTTON_H_
#define BUTTON_H_

#include <stdbool.h>
#include <stdint.h>

#include <driver/gpio.h>
#include <iot_button.h>
#include <button_types.h>
#include <button_adc.h>
#include <button_gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct board_btn board_btn_t;

typedef void (*board_btn_callback_fn)(void *user_data);

typedef struct {
    gpio_num_t gpio_num;
    bool active_high;
    uint16_t long_press_time;
    uint16_t short_press_time;
    bool enable_power_save;
} board_btn_gpio_cfg_t;

board_btn_t *board_btn_create_gpio(const board_btn_gpio_cfg_t *cfg);
board_btn_t *board_btn_create_from_handle(button_handle_t handle);
#if CONFIG_SOC_ADC_SUPPORTED
board_btn_t *board_btn_create_adc(const button_adc_config_t *adc_cfg);
#endif
void board_btn_delete(board_btn_t *btn);

void board_btn_on_press_down(board_btn_t *btn, board_btn_callback_fn cb, void *user_data);
void board_btn_on_press_up(board_btn_t *btn, board_btn_callback_fn cb, void *user_data);
void board_btn_on_long_press(board_btn_t *btn, board_btn_callback_fn cb, void *user_data);
void board_btn_on_click(board_btn_t *btn, board_btn_callback_fn cb, void *user_data);
void board_btn_on_double_click(board_btn_t *btn, board_btn_callback_fn cb, void *user_data);
void board_btn_on_multiple_click(board_btn_t *btn, board_btn_callback_fn cb, void *user_data, uint8_t click_count);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <functional>

class Button {
public:
    Button(button_handle_t button_handle)
        : handle_(button_handle ? board_btn_create_from_handle(button_handle) : nullptr) {}
    Button(gpio_num_t gpio_num, bool active_high = false,
           uint16_t long_press_time = 0, uint16_t short_press_time = 0,
           bool enable_power_save = false) {
        board_btn_gpio_cfg_t cfg = {
            .gpio_num = gpio_num,
            .active_high = active_high,
            .long_press_time = long_press_time,
            .short_press_time = short_press_time,
            .enable_power_save = enable_power_save,
        };
        handle_ = board_btn_create_gpio(&cfg);
    }
    ~Button() { if (handle_) board_btn_delete(handle_); }

    void OnPressDown(std::function<void()> callback) {
        on_press_down_ = std::move(callback);
        board_btn_on_press_down(handle_, invoke_cb<&Button::on_press_down_>, this);
    }
    void OnPressUp(std::function<void()> callback) {
        on_press_up_ = std::move(callback);
        board_btn_on_press_up(handle_, invoke_cb<&Button::on_press_up_>, this);
    }
    void OnLongPress(std::function<void()> callback) {
        on_long_press_ = std::move(callback);
        board_btn_on_long_press(handle_, invoke_cb<&Button::on_long_press_>, this);
    }
    void OnClick(std::function<void()> callback) {
        on_click_ = std::move(callback);
        board_btn_on_click(handle_, invoke_cb<&Button::on_click_>, this);
    }
    void OnDoubleClick(std::function<void()> callback) {
        on_double_click_ = std::move(callback);
        board_btn_on_double_click(handle_, invoke_cb<&Button::on_double_click_>, this);
    }
    void OnMultipleClick(std::function<void()> callback, uint8_t click_count = 3) {
        on_multiple_click_ = std::move(callback);
        board_btn_on_multiple_click(handle_, invoke_cb<&Button::on_multiple_click_>, this, click_count);
    }

protected:
    board_btn_t *handle_;
    gpio_num_t gpio_num_ = GPIO_NUM_NC;

    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
    std::function<void()> on_multiple_click_;

private:
    Button(const Button&) = delete;
    Button& operator=(const Button&) = delete;

    template<std::function<void()> Button::*Member>
    static void invoke_cb(void *user_data) {
        auto *self = static_cast<Button*>(user_data);
        auto& fn = self->*Member;
        if (fn) fn();
    }
};

#if CONFIG_SOC_ADC_SUPPORTED
class AdcButton : public Button {
public:
    AdcButton(const button_adc_config_t& adc_config)
        : Button(static_cast<button_handle_t>(nullptr)) {
        handle_ = board_btn_create_adc(&adc_config);
    }
};
#endif

class PowerSaveButton : public Button {
public:
    PowerSaveButton(gpio_num_t gpio_num) : Button(gpio_num, false, 0, 0, true) {}
};

#endif /* __cplusplus */

#endif /* BUTTON_H_ */
