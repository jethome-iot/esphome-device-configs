#include "meter_calibration.h"
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include "esphome/core/log.h"

namespace esphome::meter_calibration {

static const char *const TAG = "meter_calibration";

using jethome_board_info::field_to_string;

// What a failed read means for the record, in the words the log and the reason use.
static const char *read_reason(ReadStatus status) {
  switch (status) {
    case ReadStatus::OK:
      return "read";
    case ReadStatus::NO_EEPROM:
    case ReadStatus::READ_ERROR:
      return "the metering board's EEPROM did not answer";
    case ReadStatus::NO_HEADER:
      return "the metering board's EEPROM has no board header";
    case ReadStatus::NOT_FOUND:
      return "the metering board's EEPROM has no meter.cal";
    case ReadStatus::UNSUPPORTED:
      return "the metering board's EEPROM uses a header or filesystem version this firmware does not read";
    case ReadStatus::TOO_LARGE:
      return "meter.cal is larger than a version 1 record";
    case ReadStatus::CORRUPT:
      return "the board header, the file chain or meter.cal fails its CRC";
    case ReadStatus::BAD_NAME:
      return "meter.cal is not a name the file chain takes";
  }
  return "unknown";
}

static std::string format_time(int64_t timestamp) {
  time_t ts = static_cast<time_t>(timestamp);
  struct tm tm_info {};
  if (gmtime_r(&ts, &tm_info) == nullptr)
    return "invalid";
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm_info);
  return buf;
}

void MeterCalibration::setup() {
  MeterCalRecord rec{};
  auto *raw = reinterpret_cast<uint8_t *>(&rec);
  uint16_t size = 0;
  ReadStatus read = ReadStatus::NO_EEPROM;
  Expectation expect{this->chip_, this->board_model_, "", this->pga_code_};
  if (this->board_info_ != nullptr) {
    read = this->board_info_->read_file(RECORD_FILE_NAME, raw, sizeof(rec), &size);
    expect.board_serial = this->board_info_->get_board_serial();
  }

  this->evaluation_ = evaluate(read, raw, size, expect);
  if (this->has_record()) {
    this->record_ = rec;
  }

  if (this->evaluation_.read != ReadStatus::OK) {
    this->reason_ = read_reason(this->evaluation_.read);
  } else if (this->evaluation_.record != RecordCheck::OK) {
    this->reason_ = record_check_str(this->evaluation_.record);
  } else if (this->evaluation_.match != MatchCheck::OK) {
    this->reason_ = this->describe_(rec, expect);
  } else if (this->evaluation_.status == CalStatus::UNVERIFIED) {
    this->reason_ = "the station did not finish: the line check flag is clear";
  }
  this->log_status_();
}

// Names both sides of the mismatch, so a log line is enough to tell which board it came from.
std::string MeterCalibration::describe_(const MeterCalRecord &rec, const Expectation &expect) const {
  char buf[160];
  const char *what = match_check_str(this->evaluation_.match);
  switch (this->evaluation_.match) {
    case MatchCheck::WRONG_CHIP:
      snprintf(buf, sizeof(buf), "%s (record %s, firmware %s)", what, chip_name(rec.chip_type),
               chip_name(static_cast<uint8_t>(expect.chip)));
      break;
    case MatchCheck::WRONG_CHANNEL_COUNT:
      snprintf(buf, sizeof(buf), "%s (record %u, firmware %u)", what, rec.channel_count, channels_of(expect.chip));
      break;
    case MatchCheck::WRONG_BOARD_MODEL:
      snprintf(buf, sizeof(buf), "%s (record '%s', firmware '%s')", what,
               field_to_string(rec.board_model, sizeof(rec.board_model)).c_str(), expect.board_model.c_str());
      break;
    case MatchCheck::WRONG_BOARD_SERIAL:
      snprintf(buf, sizeof(buf), "%s (record '%s', board header '%s')", what,
               field_to_string(rec.board_serial, sizeof(rec.board_serial)).c_str(), expect.board_serial.c_str());
      break;
    case MatchCheck::PGA_MISMATCH:
      snprintf(buf, sizeof(buf), "%s (record 0x%02X, firmware 0x%02X)", what, rec.pga_code, expect.pga_code);
      break;
    case MatchCheck::NO_BOARD_SERIAL:
    case MatchCheck::OK:
    default:
      snprintf(buf, sizeof(buf), "%s", what);
      break;
  }
  return buf;
}

void MeterCalibration::log_status_() const {
  switch (this->evaluation_.status) {
    case CalStatus::FACTORY:
      ESP_LOGI(TAG, "Factory calibration of %s %s, serial '%s', from %s", this->get_board_model().c_str(),
               this->get_board_revision().c_str(), this->get_board_serial().c_str(),
               format_time(this->record_.timestamp).c_str());
      return;
    case CalStatus::MISSING:
      // Every board shipped before the station wrote records reads this way: not a fault.
      ESP_LOGI(TAG, "No factory calibration: %s", this->reason_.c_str());
      return;
    case CalStatus::UNVERIFIED:
    case CalStatus::CORRUPT:
    case CalStatus::FOREIGN:
    case CalStatus::UNSUPPORTED:
    case CalStatus::NO_EEPROM:
    case CalStatus::NOT_READ:
      ESP_LOGW(TAG, "Factory calibration %s: %s", this->get_status_str(), this->reason_.c_str());
      return;
  }
}

const Atm90e32Body *MeterCalibration::get_atm90e32() const {
  if (!this->has_record() || this->record_.chip_type != static_cast<uint8_t>(ChipType::ATM90E32))
    return nullptr;
  return &this->record_.body.atm90e32;
}

const Bl0906Body *MeterCalibration::get_bl0906() const {
  if (!this->has_record() || this->record_.chip_type != static_cast<uint8_t>(ChipType::BL0906))
    return nullptr;
  return &this->record_.body.bl0906;
}

std::string MeterCalibration::get_board_model() const {
  return field_to_string(this->record_.board_model, sizeof(this->record_.board_model));
}

std::string MeterCalibration::get_board_revision() const {
  return field_to_string(this->record_.board_revision, sizeof(this->record_.board_revision));
}

std::string MeterCalibration::get_board_serial() const {
  return field_to_string(this->record_.board_serial, sizeof(this->record_.board_serial));
}

std::string MeterCalibration::get_station_id() const {
  return field_to_string(this->record_.station_id, sizeof(this->record_.station_id));
}

std::string MeterCalibration::get_ref_instrument_id() const {
  return field_to_string(this->record_.ref_instrument_id, sizeof(this->record_.ref_instrument_id));
}

// Fixed point printed as decimals: the record keeps no floats, so neither does the log.
static void print_fixed(char *buf, size_t len, int64_t value, uint32_t scale, int digits) {
  const char *sign = value < 0 ? "-" : "";
  const uint64_t magnitude = value < 0 ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);
  snprintf(buf, len, "%s%" PRIu64 ".%0*" PRIu64, sign, magnitude / scale, digits, magnitude % scale);
}

void MeterCalibration::dump_config() {
  ESP_LOGCONFIG(TAG, "Meter calibration:");
  ESP_LOGCONFIG(TAG, "  Firmware: %s, %u channels, board '%s', PGA 0x%02X",
                chip_name(static_cast<uint8_t>(this->chip_)), channels_of(this->chip_), this->board_model_.c_str(),
                this->pga_code_);
  ESP_LOGCONFIG(TAG, "  Status: %s", this->get_status_str());
  if (!this->reason_.empty()) {
    ESP_LOGCONFIG(TAG, "  Reason: %s", this->reason_.c_str());
  }
  if (!this->has_record()) {
    return;
  }
  const MeterCalRecord &rec = this->record_;
  char volts[24];
  char amps[24];
  char ambient[16];
  print_fixed(volts, sizeof(volts), rec.ref_voltage_mv, 1000, 3);
  print_fixed(amps, sizeof(amps), rec.ref_current_ua, 1000, 3);
  print_fixed(ambient, sizeof(ambient), rec.ambient_ddeg, 10, 1);
  ESP_LOGCONFIG(TAG,
                "  Board: %s %s, serial '%s'\n"
                "  Calibrated: %s, station '%s', reference '%s', operator %" PRIu32 "\n"
                "  Reference point: %s V, %s mA secondary, %u Hz, %s C, CT %u:1\n"
                "  Flags: 0x%04X\n"
                "  Record CRC32: 0x%08" PRIX32,
                this->get_board_model().c_str(), this->get_board_revision().c_str(), this->get_board_serial().c_str(),
                format_time(rec.timestamp).c_str(), this->get_station_id().c_str(),
                this->get_ref_instrument_id().c_str(), static_cast<uint32_t>(rec.operator_code), volts, amps,
                rec.line_freq_hz, ambient, rec.ct_nominal_ratio, rec.flags, static_cast<uint32_t>(rec.crc32));

  if (const Atm90e32Body *atm = this->get_atm90e32()) {
    static const char PHASES[] = "ABC";
    for (size_t p = 0; p < ATM90E32_CHANNELS; p++) {
      const Atm90e32Phase &ph = atm->phase[p];
      ESP_LOGCONFIG(TAG, "  Phase %c: Ugain %u, Igain %u, offsets U %d I %d P %d Q %d, PQgain %d, Phi 0x%04X",
                    PHASES[p], ph.ugain, ph.igain, ph.uoffset, ph.ioffset, ph.poffset, ph.qoffset, ph.pqgain, ph.phi);
    }
    ESP_LOGCONFIG(TAG, "  Current scale: %u, PLconst: 0x%08" PRIX32 "%s", atm->current_scale,
                  static_cast<uint32_t>(atm->plconst), atm->plconst == 0 ? " (default)" : "");
  }
  if (const Bl0906Body *bl = this->get_bl0906()) {
    char kv[24];
    print_fixed(kv, sizeof(kv), bl->kv_milli, 1000, 3);
    ESP_LOGCONFIG(TAG, "  Kv: %s LSB/V", kv);
    for (size_t c = 0; c < BL0906_CHANNELS; c++) {
      char ki[24];
      char kp[24];
      char phase[16];
      char p_off[16];
      print_fixed(ki, sizeof(ki), bl->ki_milli[c], 1000, 3);
      print_fixed(kp, sizeof(kp), bl->kp_micro[c], 1000000, 6);
      print_fixed(phase, sizeof(phase), bl->phase_mdeg[c], 1000, 3);
      print_fixed(p_off, sizeof(p_off), bl->p_off_cw[c], 100, 2);
      ESP_LOGCONFIG(TAG, "  Channel %u: Ki %s, Kp %s, phase %s deg, offsets I %u mA P %s W",
                    static_cast<unsigned>(c + 1), ki, kp, phase, bl->i_off_ma[c], p_off);
    }
  }
}

}  // namespace esphome::meter_calibration
