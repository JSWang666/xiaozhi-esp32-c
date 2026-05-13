#ifndef I2C_DEVICE_H
#define I2C_DEVICE_H

#include <stdint.h>
#include <stddef.h>

#include <driver/i2c_master.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct i2c_device i2c_device_t;

i2c_device_t *i2c_device_create(i2c_master_bus_handle_t i2c_bus, uint8_t addr);
void i2c_device_destroy(i2c_device_t *dev);

void i2c_device_write_reg(i2c_device_t *dev, uint8_t reg, uint8_t value);
uint8_t i2c_device_read_reg(i2c_device_t *dev, uint8_t reg);
void i2c_device_read_regs(i2c_device_t *dev, uint8_t reg, uint8_t *buffer, size_t length);

i2c_master_dev_handle_t i2c_device_get_dev_handle(i2c_device_t *dev);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class I2cDevice {
public:
    I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr) {
        dev_ = i2c_device_create(i2c_bus, addr);
        i2c_device_ = i2c_device_get_dev_handle(dev_);
    }
    ~I2cDevice() { if (dev_) i2c_device_destroy(dev_); }

protected:
    i2c_master_dev_handle_t i2c_device_;

    void WriteReg(uint8_t reg, uint8_t value) {
        i2c_device_write_reg(dev_, reg, value);
    }
    uint8_t ReadReg(uint8_t reg) {
        return i2c_device_read_reg(dev_, reg);
    }
    void ReadRegs(uint8_t reg, uint8_t *buffer, size_t length) {
        i2c_device_read_regs(dev_, reg, buffer, length);
    }

private:
    i2c_device_t *dev_;
};

#endif /* __cplusplus */

#endif /* I2C_DEVICE_H */
