#ifndef ADC_BATTERY_MONITOR_H
#define ADC_BATTERY_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include <driver/gpio.h>
#include <adc_battery_estimation.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct adc_battery_monitor adc_battery_monitor_t;

typedef void (*adc_battery_monitor_charging_cb_t)(bool is_charging, void *user_data);

typedef struct {
    adc_unit_t adc_unit;
    adc_channel_t adc_channel;
    float upper_resistor;
    float lower_resistor;
    gpio_num_t charging_pin;
} adc_battery_monitor_cfg_t;

adc_battery_monitor_t *adc_battery_monitor_create(const adc_battery_monitor_cfg_t *cfg);
void adc_battery_monitor_delete(adc_battery_monitor_t *mon);

bool adc_battery_monitor_is_charging(adc_battery_monitor_t *mon);
bool adc_battery_monitor_is_discharging(adc_battery_monitor_t *mon);
uint8_t adc_battery_monitor_get_level(adc_battery_monitor_t *mon);
void adc_battery_monitor_on_charging_changed(adc_battery_monitor_t *mon,
                                             adc_battery_monitor_charging_cb_t cb,
                                             void *user_data);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <functional>

class AdcBatteryMonitor {
public:
    AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel,
                      float upper_resistor, float lower_resistor,
                      gpio_num_t charging_pin = GPIO_NUM_NC) {
        adc_battery_monitor_cfg_t cfg = {
            .adc_unit = adc_unit,
            .adc_channel = adc_channel,
            .upper_resistor = upper_resistor,
            .lower_resistor = lower_resistor,
            .charging_pin = charging_pin,
        };
        handle_ = adc_battery_monitor_create(&cfg);
    }
    ~AdcBatteryMonitor() {
        if (handle_) adc_battery_monitor_delete(handle_);
    }

    bool IsCharging() { return adc_battery_monitor_is_charging(handle_); }
    bool IsDischarging() { return adc_battery_monitor_is_discharging(handle_); }
    uint8_t GetBatteryLevel() { return adc_battery_monitor_get_level(handle_); }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = std::move(callback);
        adc_battery_monitor_on_charging_changed(handle_, invoke_cb, this);
    }

private:
    AdcBatteryMonitor(const AdcBatteryMonitor&) = delete;
    AdcBatteryMonitor& operator=(const AdcBatteryMonitor&) = delete;

    static void invoke_cb(bool is_charging, void *user_data) {
        auto *self = static_cast<AdcBatteryMonitor*>(user_data);
        if (self->on_charging_status_changed_) {
            self->on_charging_status_changed_(is_charging);
        }
    }

    adc_battery_monitor_t *handle_ = nullptr;
    std::function<void(bool)> on_charging_status_changed_;
};

#endif /* __cplusplus */

#endif /* ADC_BATTERY_MONITOR_H */
