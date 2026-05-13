#include "adc_battery_monitor.h"

#include <stdlib.h>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "AdcBatteryMonitor"

struct adc_battery_monitor {
    gpio_num_t charging_pin;
    adc_battery_estimation_handle_t estimation_handle;
    esp_timer_handle_t timer_handle;
    bool is_charging;
    adc_battery_monitor_charging_cb_t on_charging_changed;
    void *cb_user_data;
};

static bool charging_detect_cb(void *user_data)
{
    adc_battery_monitor_t *mon = (adc_battery_monitor_t *)user_data;
    return gpio_get_level(mon->charging_pin) == 1;
}

static void check_battery_status(adc_battery_monitor_t *mon)
{
    bool new_status = adc_battery_monitor_is_charging(mon);
    if (new_status != mon->is_charging) {
        mon->is_charging = new_status;
        if (mon->on_charging_changed) {
            mon->on_charging_changed(mon->is_charging, mon->cb_user_data);
        }
    }
}

static void timer_callback(void *arg)
{
    check_battery_status((adc_battery_monitor_t *)arg);
}

adc_battery_monitor_t *adc_battery_monitor_create(const adc_battery_monitor_cfg_t *cfg)
{
    if (!cfg) return NULL;

    adc_battery_monitor_t *mon = calloc(1, sizeof(*mon));
    if (!mon) return NULL;

    mon->charging_pin = cfg->charging_pin;

    if (mon->charging_pin != GPIO_NUM_NC) {
        gpio_config_t gpio_cfg = {
            .pin_bit_mask = 1ULL << cfg->charging_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&gpio_cfg));
    }

    adc_battery_estimation_t adc_cfg = {
        .internal = {
            .adc_unit = cfg->adc_unit,
            .adc_bitwidth = ADC_BITWIDTH_DEFAULT,
            .adc_atten = ADC_ATTEN_DB_12,
        },
        .adc_channel = cfg->adc_channel,
        .upper_resistor = cfg->upper_resistor,
        .lower_resistor = cfg->lower_resistor,
    };

    if (mon->charging_pin != GPIO_NUM_NC) {
        adc_cfg.charging_detect_cb = charging_detect_cb;
        adc_cfg.charging_detect_user_data = mon;
    } else {
        adc_cfg.charging_detect_cb = NULL;
        adc_cfg.charging_detect_user_data = NULL;
    }
    mon->estimation_handle = adc_battery_estimation_create(&adc_cfg);

    esp_timer_create_args_t timer_cfg = {
        .callback = timer_callback,
        .arg = mon,
        .name = "adc_battery_monitor",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_cfg, &mon->timer_handle));
    ESP_ERROR_CHECK(esp_timer_start_periodic(mon->timer_handle, 1000000));

    return mon;
}

void adc_battery_monitor_delete(adc_battery_monitor_t *mon)
{
    if (!mon) return;

    if (mon->timer_handle) {
        esp_timer_stop(mon->timer_handle);
        esp_timer_delete(mon->timer_handle);
    }

    if (mon->estimation_handle) {
        adc_battery_estimation_destroy(mon->estimation_handle);
    }

    free(mon);
}

bool adc_battery_monitor_is_charging(adc_battery_monitor_t *mon)
{
    if (!mon) return false;

    if (mon->estimation_handle) {
        bool is_charging = false;
        esp_err_t err = adc_battery_estimation_get_charging_state(mon->estimation_handle, &is_charging);
        if (err == ESP_OK) {
            return is_charging;
        }
    }

    if (mon->charging_pin != GPIO_NUM_NC) {
        return gpio_get_level(mon->charging_pin) == 1;
    }

    return false;
}

bool adc_battery_monitor_is_discharging(adc_battery_monitor_t *mon)
{
    return !adc_battery_monitor_is_charging(mon);
}

uint8_t adc_battery_monitor_get_level(adc_battery_monitor_t *mon)
{
    if (!mon || !mon->estimation_handle) {
        return 100;
    }

    float capacity = 0;
    esp_err_t err = adc_battery_estimation_get_capacity(mon->estimation_handle, &capacity);
    if (err != ESP_OK) {
        return 100;
    }
    return (uint8_t)capacity;
}

void adc_battery_monitor_on_charging_changed(adc_battery_monitor_t *mon,
                                             adc_battery_monitor_charging_cb_t cb,
                                             void *user_data)
{
    if (!mon) return;
    mon->on_charging_changed = cb;
    mon->cb_user_data = user_data;
}
