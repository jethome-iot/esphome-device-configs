#pragma once

#include "esphome/core/defines.h"

#ifdef USE_FRAM_STORE_I2C

#include "esphome/components/i2c_eeprom/i2c_eeprom.h"
#include "nvram_backend.h"

namespace esphome::fram_store {

// Wraps an i2c_eeprom part; it is only usable once its own setup() has found the chip.
class I2CEepromBackend : public NvramBackend {
 public:
  explicit I2CEepromBackend(i2c_eeprom::I2CEeprom *device) : device_(device) {}

  bool read(uint16_t address, uint8_t *data, size_t len) override { return this->device_->get(address, data, len); }
  bool write(uint16_t address, const uint8_t *data, size_t len) override {
    return this->device_->put(address, data, len);
  }

  uint32_t size() const override { return this->device_->get_size(); }
  bool is_ready() const override { return !this->device_->is_failed(); }
  const char *type() const override { return this->device_->is_fram() ? "FRAM" : "EEPROM"; }

 protected:
  i2c_eeprom::I2CEeprom *device_;
};

}  // namespace esphome::fram_store

#endif  // USE_FRAM_STORE_I2C
