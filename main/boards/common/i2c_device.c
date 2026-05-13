#include "i2c_device.h"

#include <stdlib.h>
#include <assert.h>
#include <esp_log.h>

#define TAG "I2cDevice"

struct i2c_device {
    i2c_master_dev_handle_t handle;
};

i2c_device_t *i2c_device_create(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
{
    i2c_device_t *dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &cfg, &dev->handle));
    assert(dev->handle != NULL);
    return dev;
}

void i2c_device_destroy(i2c_device_t *dev)
{
    if (!dev) return;
    if (dev->handle) {
        i2c_master_bus_rm_device(dev->handle);
    }
    free(dev);
}

void i2c_device_write_reg(i2c_device_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t buffer[2] = {reg, value};
    ESP_ERROR_CHECK(i2c_master_transmit(dev->handle, buffer, 2, 100));
}

uint8_t i2c_device_read_reg(i2c_device_t *dev, uint8_t reg)
{
    uint8_t buffer[1];
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev->handle, &reg, 1, buffer, 1, 100));
    return buffer[0];
}

void i2c_device_read_regs(i2c_device_t *dev, uint8_t reg, uint8_t *buffer, size_t length)
{
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev->handle, &reg, 1, buffer, length, 100));
}

i2c_master_dev_handle_t i2c_device_get_dev_handle(i2c_device_t *dev)
{
    if (!dev) return NULL;
    return dev->handle;
}
