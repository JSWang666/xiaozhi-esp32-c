#include "sy6970.h"

#include <stdlib.h>
#include <esp_log.h>

#define TAG "Sy6970"

struct sy6970 {
    i2c_device_t *i2c_dev;
};

sy6970_t *sy6970_create(i2c_master_bus_handle_t i2c_bus, uint8_t addr) {
    sy6970_t *dev = (sy6970_t *)calloc(1, sizeof(sy6970_t));
    if (!dev) {
        return NULL;
    }
    dev->i2c_dev = i2c_device_create(i2c_bus, addr);
    if (!dev->i2c_dev) {
        free(dev);
        return NULL;
    }
    return dev;
}

void sy6970_destroy(sy6970_t *dev) {
    if (dev) {
        if (dev->i2c_dev) {
            i2c_device_destroy(dev->i2c_dev);
        }
        free(dev);
    }
}

int sy6970_get_charging_status(sy6970_t *dev) {
    return (i2c_device_read_reg(dev->i2c_dev, 0x0B) >> 3) & 0x03;
}

bool sy6970_is_charging(sy6970_t *dev) {
    return sy6970_get_charging_status(dev) != 0;
}

bool sy6970_is_power_good(sy6970_t *dev) {
    return (i2c_device_read_reg(dev->i2c_dev, 0x0B) & 0x04) != 0;
}

bool sy6970_is_charging_done(sy6970_t *dev) {
    return sy6970_get_charging_status(dev) == 3;
}

int sy6970_get_battery_voltage(sy6970_t *dev) {
    uint8_t value = i2c_device_read_reg(dev->i2c_dev, 0x0E);
    value &= 0x7F;
    if (value == 0) {
        return 0;
    }
    return value * 20 + 2304;
}

int sy6970_get_charge_target_voltage(sy6970_t *dev) {
    uint8_t value = i2c_device_read_reg(dev->i2c_dev, 0x06);
    value = (value & 0xFC) >> 2;
    if (value > 0x30) {
        return 4608;
    }
    return value * 16 + 3840;
}

int sy6970_get_battery_level(sy6970_t *dev) {
    int level = 0;
    int battery_minimum_voltage = 3200;
    int battery_voltage = sy6970_get_battery_voltage(dev);
    int charge_voltage_limit = sy6970_get_charge_target_voltage(dev);
    if (battery_voltage > battery_minimum_voltage && charge_voltage_limit > battery_minimum_voltage) {
        level = (int)(((float)battery_voltage - (float)battery_minimum_voltage) /
                      ((float)charge_voltage_limit - (float)battery_minimum_voltage) * 100.0f);
    }
    if (level > 100) {
        level = 100;
    }
    return level;
}

void sy6970_power_off(sy6970_t *dev) {
    i2c_device_write_reg(dev->i2c_dev, 0x09, 0x64);
}
