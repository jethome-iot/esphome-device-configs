#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::fram_store {

// Byte-addressable non-volatile memory, as FramStore needs it: no pages, no erase, no wear budget.
class NvramBackend {
 public:
  virtual ~NvramBackend() = default;

  virtual bool read(uint16_t address, uint8_t *data, size_t len) = 0;
  virtual bool write(uint16_t address, const uint8_t *data, size_t len) = 0;
  virtual uint32_t size() const = 0;
  virtual bool is_ready() const = 0;
  virtual const char *type() const = 0;
};

// Volatile stand-in for the host tests and for a build that declares no part.
class RamBackend : public NvramBackend {
 public:
  explicit RamBackend(uint32_t size) : data_(size, 0) {}

  bool read(uint16_t address, uint8_t *data, size_t len) override {
    if (!this->in_range_(address, len))
      return false;
    std::copy(this->data_.begin() + address, this->data_.begin() + address + len, data);
    return true;
  }

  bool write(uint16_t address, const uint8_t *data, size_t len) override {
    if (!this->in_range_(address, len))
      return false;
    std::copy(data, data + len, this->data_.begin() + address);
    return true;
  }

  uint32_t size() const override { return this->data_.size(); }
  bool is_ready() const override { return true; }
  const char *type() const override { return "RAM"; }

 protected:
  bool in_range_(uint16_t address, size_t len) const {
    return address < this->data_.size() && len <= this->data_.size() - address;
  }

  std::vector<uint8_t> data_;
};

}  // namespace esphome::fram_store
