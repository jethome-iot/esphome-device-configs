#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <ostream>
#include <string>
#include <vector>
#include "esphome/components/i2c_eeprom/i2c_eeprom.h"
#include "esphome/components/jethome_board_info/jethome_board_info.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/meter_calibration/meter_calibration.h"
#include "meter_vectors.h"

// Found by gtest through ADL, so a failed comparison names the value instead of a byte.
namespace esphome::jethome_board_info {
inline void PrintTo(ReadStatus status, std::ostream *os) { *os << read_status_str(status); }
}  // namespace esphome::jethome_board_info

namespace esphome::meter_calibration {
inline void PrintTo(CalStatus status, std::ostream *os) { *os << cal_status_str(status); }
inline void PrintTo(RecordCheck check, std::ostream *os) { *os << record_check_str(check); }
inline void PrintTo(MatchCheck check, std::ostream *os) { *os << match_check_str(check); }
}  // namespace esphome::meter_calibration

namespace esphome::meter_calibration::testing {

using jethome_board_info::JetHomeBoardInfo;
using jethome_board_info::testing::FileSpec;
using jethome_board_info::testing::HDR_OFF_BOARD_SERIAL;
using jethome_board_info::testing::HDR_OFF_BOARDNAME;
using jethome_board_info::testing::HDR_OFF_BOARDVERSION;
using jethome_board_info::testing::HDR_OFF_CPUID;
using jethome_board_info::testing::HDR_OFF_CRC32;
using jethome_board_info::testing::HDR_OFF_FS_VERSION;
using jethome_board_info::testing::HDR_OFF_MAC;
using jethome_board_info::testing::HDR_OFF_TIMESTAMP;
using jethome_board_info::testing::HDR_OFF_USID;
using jethome_board_info::testing::HDR_OFF_VERSION;
using jethome_board_info::testing::make_image;

// The metering board's 64 Kbit EEPROM at 0x56: a two-byte write sets its address pointer, a
// read streams from there.
class FakeEepromBus : public i2c::I2CBus {
 public:
  std::vector<uint8_t> image;
  uint8_t address{0x56};
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
      read_buffer[i] = this->pointer_ < this->image.size() ? this->image[this->pointer_] : 0xFF;
      this->pointer_++;
    }
    if (read_count != 0)
      this->reads++;
    return i2c::ERROR_OK;
  }

 protected:
  size_t pointer_{0};
};

// Every line logged since clear(), by level. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  std::vector<std::string> infos;
  std::vector<std::string> configs;

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
    this->infos.clear();
    this->configs.clear();
  }
  bool has_error(const char *needle) const { return has(this->errors, needle); }
  bool has_warning(const char *needle) const { return has(this->warnings, needle); }
  bool has_info(const char *needle) const { return has(this->infos, needle); }
  bool has_config(const char *needle) const { return has(this->configs, needle); }

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
    } else if (level == ESPHOME_LOG_LEVEL_INFO) {
      capture->infos.emplace_back(message, len);
    } else if (level == ESPHOME_LOG_LEVEL_CONFIG) {
      capture->configs.emplace_back(message, len);
    }
  }
};

// A board header the way the station writes it on 0x56: board-scoped v4, the metering board's
// own name, revision and serial, no USID, CPU id or MAC.
inline void stamp_meter_header(std::vector<uint8_t> &image, const BoardIdentity &board) {
  uint8_t *raw = image.data();
  memset(raw + HDR_OFF_BOARDNAME, 0, 32 * 5 + 6);
  put_field(raw, HDR_OFF_BOARDNAME, 32, board.model);
  put_field(raw, HDR_OFF_BOARDVERSION, 32, board.revision);
  put_field(raw, HDR_OFF_BOARD_SERIAL, 32, board.serial);
  put_u32(raw, HDR_OFF_CRC32, jethome_board_info::compute_crc32(raw, jethome_board_info::CRC_DATA_SIZE));
}

inline void reseal_header(std::vector<uint8_t> &image) {
  put_u32(image.data(), HDR_OFF_CRC32,
          jethome_board_info::compute_crc32(image.data(), jethome_board_info::CRC_DATA_SIZE));
}

inline FileSpec meter_cal_file(const uint8_t *data, size_t size) {
  return FileSpec{RECORD_FILE_NAME, std::vector<uint8_t>(data, data + size)};
}

inline FileSpec meter_cal_file(const MeterRecordBuf &record) { return meter_cal_file(record.data(), record.size()); }

// The whole 0x56 part: header, then the given files, then erased bytes.
inline std::vector<uint8_t> meter_image(const std::vector<FileSpec> &files, const BoardIdentity &board = PM33) {
  std::vector<uint8_t> image = make_image(files);
  size_t end = jethome_board_info::FILE_HEADER_ADDR;
  for (const FileSpec &f : files)
    end += jethome_board_info::FILE_HEADER_SIZE + f.data.size();
  std::fill(image.begin() + end, image.end(), 0xFF);
  stamp_meter_header(image, board);
  return image;
}

inline std::vector<uint8_t> meter_image(const MeterRecordBuf &record, const BoardIdentity &board = PM33) {
  return meter_image({meter_cal_file(record)}, board);
}

// What the firmware of each meter expects.
inline Expectation pm380() { return Expectation{ChipType::ATM90E32, PM33.model, PM33.serial, 0x15}; }
inline Expectation pm220() { return Expectation{ChipType::BL0906, PM16.model, PM16.serial, 0x00}; }

// A metering board on its own bus: the EEPROM, its optional board info and the loader.
struct MeterBoard {
  FakeEepromBus bus;
  i2c_eeprom::I2CEeprom eeprom;
  JetHomeBoardInfo info;
  MeterCalibration cal;

  explicit MeterBoard(std::vector<uint8_t> image, ChipType chip = ChipType::ATM90E32) {
    this->bus.image = std::move(image);
    this->eeprom.set_i2c_bus(&this->bus);
    this->eeprom.set_i2c_address(this->bus.address);
    this->eeprom.set_size(8192);
    this->info.set_eeprom(&this->eeprom);
    this->info.set_required(false);
    const bool atm = chip == ChipType::ATM90E32;
    this->cal.set_board_info(&this->info);
    this->cal.set_chip(chip);
    this->cal.set_board_model(atm ? PM33.model : PM16.model);
    this->cal.set_pga_code(atm ? 0x15 : 0x00);
    LogCapture::instance().clear();
  }
  // In setup-priority order: the board info, then the loader.
  void boot() {
    this->info.setup();
    this->cal.setup();
  }
  const LogCapture &log() const { return LogCapture::instance(); }
};

}  // namespace esphome::meter_calibration::testing
