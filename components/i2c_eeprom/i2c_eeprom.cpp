#include "i2c_eeprom.h"
#include <algorithm>
#include <cstring>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::i2c_eeprom {

static const char *const TAG = "i2c_eeprom";

void I2CEeprom::setup() {
  if (!this->is_connected()) {
    ESP_LOGE(TAG, "No %s at 0x%02X", this->fram_ ? "FRAM" : "EEPROM", this->address_);
    this->mark_failed();
    return;
  }
  this->setup_trigger_.trigger();
}

void I2CEeprom::dump_config() {
  ESP_LOGCONFIG(TAG, "%s:", this->fram_ ? "FRAM" : "EEPROM");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Size: %u bytes", static_cast<unsigned>(this->size_));
  if (!this->fram_)
    ESP_LOGCONFIG(TAG, "  Page size: %u bytes", static_cast<unsigned>(this->page_size_));
  ESP_LOGCONFIG(TAG, "  Write protection: %s", ONOFF(this->write_protected_));
}

bool I2CEeprom::is_connected() {
  uint8_t byte = 0;
  return this->read(&byte, 1) == i2c::ERROR_OK;
}

size_t I2CEeprom::address_bytes_(uint16_t memaddr, uint8_t *out) const {
  if (this->two_byte_address_) {
    out[0] = memaddr >> 8;
    out[1] = memaddr & 0xFF;
    return 2;
  }
  out[0] = memaddr & 0xFF;
  return 1;
}

// One address byte reaches 256 bytes; the block-select bits a bigger one-byte part wants in
// its device address are not driven, so past that a request is refused, not aliased.
bool I2CEeprom::in_range_(uint16_t memaddr, size_t size) const {
  const uint32_t reach = this->two_byte_address_ ? this->size_ : std::min<uint32_t>(this->size_, 256);
  if (memaddr + size <= reach)
    return true;
  ESP_LOGE(TAG, "%u bytes at 0x%04X are past the %u bytes this part is addressed in", static_cast<unsigned>(size),
           memaddr, static_cast<unsigned>(reach));
  return false;
}

bool I2CEeprom::put(uint16_t memaddr, const uint8_t *value, size_t size) {
  if (this->write_protected_) {
    ESP_LOGE(TAG, "Write-protected: refusing %u bytes at 0x%04X", static_cast<unsigned>(size), memaddr);
    return false;
  }
  if (!this->in_range_(memaddr, size))
    return false;
  uint8_t frame[2 + MAX_TRANSFER];
  for (size_t done = 0; done < size;) {
    const uint16_t addr = static_cast<uint16_t>(memaddr + done);
    size_t chunk = std::min(size - done, MAX_TRANSFER);
    // A page write that runs past the end of its page rolls over to the first byte of that same
    // page instead of carrying into the next one, so every chunk stops at the next boundary.
    if (!this->fram_)
      chunk = std::min<size_t>(chunk, this->page_size_ - (addr % this->page_size_));
    const size_t n = this->address_bytes_(addr, frame);
    memcpy(frame + n, value + done, chunk);
    const i2c::ErrorCode err = this->write(frame, n + chunk);
    if (!this->fram_)
      delay(5);  // the write cycle
    if (err != i2c::ERROR_OK) {
      ESP_LOGE(TAG, "Writing %u bytes at 0x%04X failed: %d", static_cast<unsigned>(chunk), addr, err);
      return false;
    }
    done += chunk;
  }
  return true;
}

// Two transactions per chunk, address then data: the chip keeps its address counter across the
// stop. An EEPROM read stays one transaction, as before FRAM support; FRAM reads are cut like its
// writes, since fram_store reads payloads of up to 4 KiB, which would near the driver's timeout.
bool I2CEeprom::get(uint16_t memaddr, uint8_t *value, size_t size) {
  if (!this->in_range_(memaddr, size))
    return false;
  const size_t limit = this->fram_ ? MAX_TRANSFER : size;
  size_t done = 0;
  do {
    const uint16_t addr = static_cast<uint16_t>(memaddr + done);
    const size_t chunk = std::min(size - done, limit);
    uint8_t frame[2];
    const size_t n = this->address_bytes_(addr, frame);
    if (this->write(frame, n) != i2c::ERROR_OK) {
      ESP_LOGE(TAG, "Setting the address 0x%04X failed", addr);
      return false;
    }
    if (this->read(value + done, chunk) != i2c::ERROR_OK) {
      ESP_LOGE(TAG, "Reading %u bytes at 0x%04X failed", static_cast<unsigned>(chunk), addr);
      return false;
    }
    done += chunk;
  } while (done < size);
  return true;
}

}  // namespace esphome::i2c_eeprom
