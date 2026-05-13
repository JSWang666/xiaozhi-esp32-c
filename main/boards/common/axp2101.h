#ifndef __AXP2101_H__
#define __AXP2101_H__

#include "i2c_device.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct axp2101 axp2101_t;

axp2101_t *axp2101_create(i2c_master_bus_handle_t i2c_bus, uint8_t addr);
void axp2101_destroy(axp2101_t *dev);

bool axp2101_is_charging(axp2101_t *dev);
bool axp2101_is_discharging(axp2101_t *dev);
bool axp2101_is_charging_done(axp2101_t *dev);
int axp2101_get_battery_level(axp2101_t *dev);
float axp2101_get_temperature(axp2101_t *dev);
void axp2101_power_off(axp2101_t *dev);

i2c_device_t *axp2101_get_i2c_device(axp2101_t *dev);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class Axp2101 {
public:
    Axp2101(i2c_master_bus_handle_t i2c_bus, uint8_t addr) {
        dev_ = axp2101_create(i2c_bus, addr);
    }
    ~Axp2101() { if (dev_) axp2101_destroy(dev_); }

    bool IsCharging() { return axp2101_is_charging(dev_); }
    bool IsDischarging() { return axp2101_is_discharging(dev_); }
    bool IsChargingDone() { return axp2101_is_charging_done(dev_); }
    int GetBatteryLevel() { return axp2101_get_battery_level(dev_); }
    float GetTemperature() { return axp2101_get_temperature(dev_); }
    void PowerOff() { axp2101_power_off(dev_); }

protected:
    void WriteReg(uint8_t reg, uint8_t value) {
        i2c_device_write_reg(axp2101_get_i2c_device(dev_), reg, value);
    }
    uint8_t ReadReg(uint8_t reg) {
        return i2c_device_read_reg(axp2101_get_i2c_device(dev_), reg);
    }

private:
    axp2101_t *dev_;
};

#endif /* __cplusplus */

#endif /* __AXP2101_H__ */
