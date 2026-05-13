#include "axp2101.h"

#include <stdlib.h>
#include <esp_log.h>

#define TAG "Axp2101"

struct axp2101 {
    i2c_device_t *i2c_dev;
};

axp2101_t *axp2101_create(i2c_master_bus_handle_t i2c_bus, uint8_t addr) {
    axp2101_t *dev = (axp2101_t *)malloc(sizeof(axp2101_t));
    if (!dev) return NULL;
    dev->i2c_dev = i2c_device_create(i2c_bus, addr);
    if (!dev->i2c_dev) {
        free(dev);
        return NULL;
    }
    return dev;
}

void axp2101_destroy(axp2101_t *dev) {
    if (dev) {
        if (dev->i2c_dev) i2c_device_destroy(dev->i2c_dev);
        free(dev);
    }
}

static int axp2101_get_battery_current_direction(axp2101_t *dev) {
    return (i2c_device_read_reg(dev->i2c_dev, 0x01) & 0x60) >> 5;
}

bool axp2101_is_charging(axp2101_t *dev) {
    return axp2101_get_battery_current_direction(dev) == 1;
}

bool axp2101_is_discharging(axp2101_t *dev) {
    return axp2101_get_battery_current_direction(dev) == 2;
}

bool axp2101_is_charging_done(axp2101_t *dev) {
    uint8_t value = i2c_device_read_reg(dev->i2c_dev, 0x01);
    return (value & 0x07) == 0x04;
}

int axp2101_get_battery_level(axp2101_t *dev) {
    return i2c_device_read_reg(dev->i2c_dev, 0xA4);
}

float axp2101_get_temperature(axp2101_t *dev) {
    return (float)i2c_device_read_reg(dev->i2c_dev, 0xA5);
}

void axp2101_power_off(axp2101_t *dev) {
    uint8_t value = i2c_device_read_reg(dev->i2c_dev, 0x10);
    value = value | 0x01;
    i2c_device_write_reg(dev->i2c_dev, 0x10, value);
}

i2c_device_t *axp2101_get_i2c_device(axp2101_t *dev) {
    return dev->i2c_dev;
}
