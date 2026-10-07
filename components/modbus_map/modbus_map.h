#pragma once

#include <cstdint>
#include <vector>

namespace esphome::modbus_map {

/// A run of consecutive bit addresses that mean one thing, e.g. relays 1-6.
struct BitRange {
  uint16_t address;
  uint16_t last_address;
  // All of 0x0000-0xFFFF is 65536 values.
  uint32_t count;
  bool writable;
  const char *name;
};

/// The same for registers, with how a value is encoded. last_address covers every word of the
/// last value.
struct RegisterRange {
  uint16_t address;
  uint16_t last_address;
  uint32_t count;
  bool writable;
  const char *name;
  const char *value_type;
  float scale{0.0f};  // 0 when not given: the build refuses 0
  const char *unit{nullptr};
  int32_t no_value{-1};  // -1 when not given
};

/// A modbus_server's address map as the build derived it from the server's config.
class ModbusMap {
 public:
  void add_bits(const BitRange &range) { this->bits_.push_back(range); }
  void add_registers(const RegisterRange &range) { this->registers_.push_back(range); }
  /// Unmapped registers up to @p last_address read @p value instead of answering exception 02.
  void set_courtesy_response(uint16_t last_address, uint16_t value) {
    this->has_courtesy_response_ = true;
    this->courtesy_last_address_ = last_address;
    this->courtesy_value_ = value;
  }

  const std::vector<BitRange> &bits() const { return this->bits_; }
  const std::vector<RegisterRange> &registers() const { return this->registers_; }
  bool has_courtesy_response() const { return this->has_courtesy_response_; }
  uint16_t courtesy_last_address() const { return this->courtesy_last_address_; }
  uint16_t courtesy_value() const { return this->courtesy_value_; }

 protected:
  std::vector<BitRange> bits_;
  std::vector<RegisterRange> registers_;
  bool has_courtesy_response_{false};
  uint16_t courtesy_last_address_{0};
  uint16_t courtesy_value_{0};
};

}  // namespace esphome::modbus_map
