#pragma once

#include <cstdint>
#include <utility>
#include <vector>
#include "esphome/components/one_wire/one_wire_bus.h"
#include "esphome/core/component.h"

namespace esphome::one_wire_host {

// A 1-Wire bus with no wire behind it. The devices the boot scan found are whatever the case sets;
// a read answers all ones, which never passes a scratch pad's checksum, so every device on it is
// there but silent.
class HostOneWireBus : public one_wire::OneWireBus, public Component {
 public:
  /// What the boot scan found, in bus order.
  void set_devices(std::vector<uint64_t> devices) { this->devices_ = std::move(devices); }

  void write8(uint8_t) override {}
  void write64(uint64_t) override {}
  uint8_t read8() override { return 0xFF; }
  uint64_t read64() override { return ~uint64_t{0}; }

 protected:
  int reset_int() override { return this->devices_.empty() ? 0 : 1; }
  void reset_search() override {}
  uint64_t search_int() override { return 0; }
};

}  // namespace esphome::one_wire_host
