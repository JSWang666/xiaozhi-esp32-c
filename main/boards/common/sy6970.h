#ifndef SY6970_H
#define SY6970_H

#include "i2c_device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sy6970 sy6970_t;

sy6970_t *sy6970_create(i2c_master_bus_handle_t i2c_bus, uint8_t addr);
void sy6970_destroy(sy6970_t *dev);

int sy6970_get_charging_status(sy6970_t *dev);
bool sy6970_is_charging(sy6970_t *dev);
bool sy6970_is_power_good(sy6970_t *dev);
bool sy6970_is_charging_done(sy6970_t *dev);
int sy6970_get_battery_voltage(sy6970_t *dev);
int sy6970_get_charge_target_voltage(sy6970_t *dev);
int sy6970_get_battery_level(sy6970_t *dev);
void sy6970_power_off(sy6970_t *dev);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class Sy6970 : public I2cDevice {
public:
    Sy6970(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
        : I2cDevice(i2c_bus, addr)
        , sy6970_(sy6970_create(i2c_bus, addr)) {}
    ~Sy6970() { if (sy6970_) sy6970_destroy(sy6970_); }

    bool IsCharging() { return sy6970_is_charging(sy6970_); }
    bool IsPowerGood() { return sy6970_is_power_good(sy6970_); }
    bool IsChargingDone() { return sy6970_is_charging_done(sy6970_); }
    int GetBatteryLevel() { return sy6970_get_battery_level(sy6970_); }
    void PowerOff() { sy6970_power_off(sy6970_); }

private:
    sy6970_t *sy6970_;

    int GetChangingStatus() { return sy6970_get_charging_status(sy6970_); }
    int GetBatteryVoltage() { return sy6970_get_battery_voltage(sy6970_); }
    int GetChargeTargetVoltage() { return sy6970_get_charge_target_voltage(sy6970_); }
};

#endif /* __cplusplus */

#endif /* SY6970_H */
