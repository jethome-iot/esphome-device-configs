#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include "esphome/components/i2c_eeprom/i2c_eeprom.h"
#include "esphome/components/jethome_board_info/jethome_board_info.h"
#include "esphome/components/logger/logger.h"
#include "jeefs_vectors.h"

namespace esphome::jethome_board_info::testing {

// One 64 Kbit EEPROM on the bus: a two-byte write sets its address pointer, a read streams
// from there. Anything else on the wire is refused, as a bus with one chip refuses it.
class FakeEepromBus : public i2c::I2CBus {
 public:
  std::vector<uint8_t> image;
  uint8_t address{0x54};
  bool present{true};
  // Reads answered before the chip stops answering; -1 for never.
  int fail_after_reads{-1};
  int reads{0};

  i2c::ErrorCode write_readv(uint8_t addr, const uint8_t *write_buffer, size_t write_count, uint8_t *read_buffer,
                             size_t read_count) override {
    if (!this->present || addr != this->address)
      return i2c::ERROR_NOT_ACKNOWLEDGED;
    if (read_count != 0 && this->fail_after_reads >= 0 && this->reads >= this->fail_after_reads)
      return i2c::ERROR_TIMEOUT;
    if (write_count != 0) {
      if (write_count != 2)
        return i2c::ERROR_INVALID_ARGUMENT;
      this->pointer_ = (static_cast<size_t>(write_buffer[0]) << 8) | write_buffer[1];
    }
    for (size_t i = 0; i < read_count; i++) {
      read_buffer[i] = this->pointer_ < this->image.size() ? this->image[this->pointer_] : ERASED_BYTE;
      this->pointer_++;
    }
    if (read_count != 0)
      this->reads++;
    return i2c::ERROR_OK;
  }

 protected:
  size_t pointer_{0};
};

// Every error and warning logged since clear(). Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() {
    this->errors.clear();
    this->warnings.clear();
  }
  bool has_error(const char *needle) const { return has(this->errors, needle); }
  bool has_warning(const char *needle) const { return has(this->warnings, needle); }

 protected:
  static bool has(const std::vector<std::string> &lines, const char *needle) {
    return std::any_of(lines.begin(), lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    auto *capture = static_cast<LogCapture *>(self);
    if (level == ESPHOME_LOG_LEVEL_ERROR) {
      capture->errors.emplace_back(message, len);
    } else if (level == ESPHOME_LOG_LEVEL_WARN) {
      capture->warnings.emplace_back(message, len);
    }
  }
};

// A board: the image in its EEPROM, the eeprom component over the fake bus, and the
// component under test.
struct Board {
  FakeEepromBus bus;
  i2c_eeprom::I2CEeprom eeprom;
  JetHomeBoardInfo info;

  explicit Board(std::vector<uint8_t> image) {
    this->bus.image = std::move(image);
    this->eeprom.set_i2c_bus(&this->bus);
    this->eeprom.set_i2c_address(this->bus.address);
    this->eeprom.set_size(8192);
    this->info.set_eeprom(&this->eeprom);
    LogCapture::instance().clear();
  }
  // The signature the component publishes, as a vector for the comparisons.
  std::vector<uint8_t> signature() const {
    const uint8_t *sig = this->info.get_signature();
    return std::vector<uint8_t>(sig, sig + ECDSA_P256_SIG_SIZE);
  }
  void boot() { this->info.setup(); }
  const LogCapture &log() const { return LogCapture::instance(); }
};

// Recomputes the header CRC after a test has changed a field under it.
inline void reseal_header(std::vector<uint8_t> &image) {
  put_u32(image.data(), HDR_OFF_CRC32, compute_crc32(image.data(), CRC_DATA_SIZE));
}

// The v3 vector as a whole EEPROM: no file area, the device serial and a signature in the header.
inline std::vector<uint8_t> v3_image() {
  std::vector<uint8_t> image(8192, 0);
  memcpy(image.data(), V3_HEADER_SECP256R1.data(), V3_HEADER_SECP256R1.size());
  return image;
}

// A record signed with secp256r1, the signature bytes all `fill` so a test can tell it apart.
inline RecordBuf signed_record(uint8_t fill) {
  RecordBuf record = make_record(DEVID_RECORD_VERSION, SIG_SECP256R1);
  memset(record.data() + REC_OFF_SIGNATURE, fill, ECDSA_P256_SIG_SIZE);
  seal_record(record);
  return record;
}

// A v4 board with the record as its device.id; the header claims `header_signature` with
// 0xCD bytes, so a test can tell which of the two the component picked.
inline std::vector<uint8_t> v4_image(const RecordBuf &record, uint8_t header_signature = SIG_NONE) {
  std::vector<uint8_t> image = make_image({device_id_file(record)});
  image[HDR_OFF_SIG_VERSION] = header_signature;
  memset(image.data() + HDR_OFF_SIGNATURE, 0xCD, ECDSA_P256_SIG_SIZE);
  reseal_header(image);
  return image;
}

}  // namespace esphome::jethome_board_info::testing
