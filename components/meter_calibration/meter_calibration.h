#pragma once
#include <cstdint>
#include <string>
#include "esphome/components/jethome_board_info/jethome_board_info.h"
#include "esphome/core/component.h"
#include "meter_cal_parse.h"

namespace esphome::meter_calibration {

// Reads meter.cal from the metering board's EEPROM once at boot and says whether its
// coefficients belong to this board. Nothing here touches a metering chip.
class MeterCalibration : public Component {
 public:
  void setup() override;
  void dump_config() override;
  // After the metering board's jethome_board_info (DATA - 1). A driver that takes the record
  // must set up below this priority; nothing here orders it.
  float get_setup_priority() const override { return setup_priority::DATA - 1.5f; }

  void set_board_info(jethome_board_info::JetHomeBoardInfo *board_info) { this->board_info_ = board_info; }
  void set_chip(ChipType chip) { this->chip_ = chip; }
  void set_board_model(const std::string &board_model) { this->board_model_ = board_model; }
  void set_pga_code(uint8_t pga_code) { this->pga_code_ = pga_code; }

  CalStatus get_status() const { return this->evaluation_.status; }
  const char *get_status_str() const { return cal_status_str(this->evaluation_.status); }
  // Why the status is not factory, in words; empty when it is.
  const std::string &get_reason() const { return this->reason_; }
  ReadStatus get_read_status() const { return this->evaluation_.read; }
  RecordCheck get_record_check() const { return this->evaluation_.record; }
  MatchCheck get_match_check() const { return this->evaluation_.match; }

  // The record is this board's and sound: factory, or unverified, which is used with a warning.
  bool has_record() const {
    return this->evaluation_.status == CalStatus::FACTORY || this->evaluation_.status == CalStatus::UNVERIFIED;
  }
  bool is_verified() const { return this->evaluation_.status == CalStatus::FACTORY; }
  // All zeroes unless has_record().
  const MeterCalRecord &get_record() const { return this->record_; }
  // The chip's part of the record; nullptr without a record or for the other chip.
  const Atm90e32Body *get_atm90e32() const;
  const Bl0906Body *get_bl0906() const;

  std::string get_board_model() const;
  std::string get_board_revision() const;
  std::string get_board_serial() const;
  std::string get_station_id() const;
  std::string get_ref_instrument_id() const;
  int64_t get_timestamp() const { return this->record_.timestamp; }
  uint16_t get_flags() const { return this->record_.flags; }
  // Identifies the factory record a later field calibration was made over.
  uint32_t get_record_crc32() const { return this->record_.crc32; }

 protected:
  std::string describe_(const MeterCalRecord &rec, const Expectation &expect) const;
  void log_status_() const;

  jethome_board_info::JetHomeBoardInfo *board_info_{nullptr};
  ChipType chip_{ChipType::ATM90E32};
  std::string board_model_;
  uint8_t pga_code_{0};

  Evaluation evaluation_{};
  MeterCalRecord record_{};
  std::string reason_;
};

}  // namespace esphome::meter_calibration
