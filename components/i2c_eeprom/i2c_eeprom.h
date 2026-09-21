#pragma once
#include <cstddef>
#include <cstdint>
#include "esphome/components/i2c/i2c.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

namespace esphome::i2c_eeprom {

class I2CEeprom : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // The chip answers a read: the only probe an EEPROM has.
  bool is_connected();

  // Split at the page boundaries the part rolls over on, one transaction per page, so a write
  // is no longer all-or-nothing: a failed chunk stops it with the chunks before it written.
  bool put(uint16_t memaddr, const uint8_t *value, size_t size);
  bool put(uint16_t memaddr, uint8_t value) { return this->put(memaddr, &value, 1); }
  bool get(uint16_t memaddr, uint8_t *value, size_t size = 1);

  // While on, every put is refused; reads are untouched. jethome_board_info turns it on for
  // the CPU board's EEPROM, which holds what the manufacturer wrote.
  void set_write_protected(bool value) { this->write_protected_ = value; }
  bool is_write_protected() const { return this->write_protected_; }

  // Bytes. Parts above 16 Kbit take a two-byte memory address; 4 to 16 Kbit parts select
  // their upper blocks through the device address, which is not driven, so only the first
  // 256 bytes of those are reached.
  void set_size(uint32_t size) {
    this->size_ = size;
    this->two_byte_address_ = size > 2048;
  }
  uint32_t get_size() const { return this->size_; }

  // Bytes per page write. The density does not give it away — a 2 Kbit AT24C02 pages 8 bytes
  // and an M24C02 16 — so the default is the smallest any 24Cxx uses; every page size is a
  // power of two, so a chunk cut on an 8-byte boundary always lies inside one real page. The
  // schema cannot pass 0, a lambda can, and put() divides by it.
  void set_page_size(uint16_t size) { this->page_size_ = size > 0 ? size : 1; }
  uint16_t get_page_size() const { return this->page_size_; }

  Trigger<> *get_setup_trigger() { return &this->setup_trigger_; }

 protected:
  size_t address_bytes_(uint16_t memaddr, uint8_t *out) const;
  bool in_range_(uint16_t memaddr, size_t size) const;

  uint32_t size_{0};
  uint16_t page_size_{8};
  bool two_byte_address_{false};
  bool write_protected_{false};
  Trigger<> setup_trigger_;
};

}  // namespace esphome::i2c_eeprom
