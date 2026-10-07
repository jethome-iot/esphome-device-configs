#pragma once
// The factory calibration record, meter.cal, byte by byte. Free of ESPHome and I2C like
// jeefs_parse.h, so the host tests in tests/components/meter_calibration/ include it on its own.
// The layout is the one components/meter_calibration/README.md specifies for the factory station.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "esphome/components/jethome_board_info/jeefs_parse.h"

namespace esphome::meter_calibration {

using jethome_board_info::ReadStatus;

static const char RECORD_FILE_NAME[] = "meter.cal";
static const size_t RECORD_SIZE = 256;
static const size_t RECORD_CRC_DATA_SIZE = 252;
static const size_t RECORD_MAGIC_SIZE = 8;
// Six letters, two NULs: all eight bytes are compared.
static const uint8_t RECORD_MAGIC[RECORD_MAGIC_SIZE] = {'J', 'H', 'M', 'C', 'A', 'L', 0, 0};
static const uint8_t RECORD_VERSION = 1;
// Reserved for a signed record; nothing past it is defined yet.
static const uint8_t SIGNATURE_NONE = 0;
// The coefficients are referred to the primary current of a CT of this ratio.
static const uint16_t CT_NOMINAL_RATIO = 2000;

enum class ChipType : uint8_t {
  ATM90E32 = 1,
  BL0906 = 2,
};

// Current channels each chip has on its board.
static const uint8_t ATM90E32_CHANNELS = 3;
static const uint8_t BL0906_CHANNELS = 6;

// Which groups of coefficients the station calibrated; bit 5 says the check on the line passed.
static const uint16_t FLAG_GAIN_UI = 1u << 0;
static const uint16_t FLAG_OFFSET_UI = 1u << 1;
static const uint16_t FLAG_OFFSET_PQ = 1u << 2;
static const uint16_t FLAG_GAIN_ENERGY = 1u << 3;
static const uint16_t FLAG_PHASE = 1u << 4;
static const uint16_t FLAG_LINE_VERIFIED = 1u << 5;
static const uint16_t FLAGS_DEFINED = 0x003F;

// The register codes the station wrote for one phase, as the chip takes them.
struct __attribute__((packed)) Atm90e32Phase {
  uint16_t ugain;   // 0
  uint16_t igain;   // 2
  int16_t uoffset;  // 4
  int16_t ioffset;  // 6
  int16_t poffset;  // 8
  int16_t qoffset;  // 10
  int16_t pqgain;   // 12
  uint16_t phi;     // 14: bit 15 DelayV, bits 7:0 cycles, bits 14:8 zero
};

static_assert(sizeof(Atm90e32Phase) == 16, "Atm90e32Phase must be 16 bytes");

// Record offsets 128-239.
struct __attribute__((packed)) Atm90e32Body {
  Atm90e32Phase phase[3];  // 128-175: A, B, C
  uint8_t current_scale;   // 176: 1, or 2 for gains taken at half scale
  uint8_t reserved1;       // 177
  uint32_t plconst;        // 178: 0 = the firmware's default
  int16_t err_pf1[3];      // 182: 0.01 %
  int16_t err_pf05l[3];    // 188: 0.01 %
  int16_t err_worst[3];    // 194: 0.01 %
  uint8_t reserved2[40];   // 200-239
};

static_assert(sizeof(Atm90e32Body) == 112, "Atm90e32Body must be 112 bytes");

// Record offsets 128-239. Physical coefficients in fixed point, not register codes.
struct __attribute__((packed)) Bl0906Body {
  uint32_t kv_milli;      // 128: LSB per volt x 1000
  uint32_t ki_milli[6];   // 132: LSB per primary ampere at the nominal ratio x 1000
  uint32_t kp_micro[6];   // 156: LSB per watt x 10^6
  int16_t phase_mdeg[6];  // 180: 0.001 degree
  uint8_t reserved1[2];   // 192
  uint16_t i_off_ma[6];   // 194: primary mA, subtracted in squares
  int16_t p_off_cw[6];    // 206: 0.01 W
  int16_t err_worst[6];   // 218: 0.01 %
  uint8_t reserved2[10];  // 230-239
};

static_assert(sizeof(Bl0906Body) == 112, "Bl0906Body must be 112 bytes");

struct __attribute__((packed)) MeterCalRecord {
  uint8_t magic[8];            // 0-7: "JHMCAL\0\0"
  uint8_t record_version;      // 8
  uint8_t signature_version;   // 9
  uint8_t chip_type;           // 10
  uint8_t channel_count;       // 11
  char board_model[32];        // 12-43
  char board_revision[8];      // 44-51
  char board_serial[32];       // 52-83
  uint32_t ref_voltage_mv;     // 84
  uint32_t ref_current_ua;     // 88: secondary
  uint16_t ct_nominal_ratio;   // 92
  uint8_t line_freq_hz;        // 94
  uint8_t pga_code;            // 95
  int16_t ambient_ddeg;        // 96: 0.1 degree C
  uint16_t flags;              // 98
  char station_id[12];         // 100-111
  char ref_instrument_id[16];  // 112-127
  union {
    Atm90e32Body atm90e32;
    Bl0906Body bl0906;
    uint8_t raw[112];
  } body;                  // 128-239
  int64_t timestamp;       // 240: Unix time, UTC
  uint32_t operator_code;  // 248
  uint32_t crc32;          // 252: over bytes 0-251
};

static_assert(sizeof(MeterCalRecord) == RECORD_SIZE, "MeterCalRecord must be 256 bytes");

// What the record says about itself, checked in this order; the first failure is the answer.
enum class RecordCheck : uint8_t {
  OK,
  BAD_SIZE,  // too short to carry a version, or not 256 bytes at version 1
  BAD_MAGIC,
  UNSUPPORTED_VERSION,
  BAD_CRC,
  UNSUPPORTED_SIGNATURE,
  UNKNOWN_CHIP,
  UNKNOWN_FLAGS,      // a flag bit version 1 does not define
  RESERVED_NOT_ZERO,  // a reserved byte version 1 leaves zero
  UNSUPPORTED_CT_RATIO,
  BAD_LINE_FREQUENCY,
  BAD_CURRENT_SCALE,
  BAD_PHI,    // phi bits 14:8 set
  ZERO_GAIN,  // a gain the flags say was calibrated is zero
};

// What the firmware was built for, against which a sound record is matched.
struct Expectation {
  ChipType chip;
  std::string board_model;
  // The serial in the metering board's own header: a record copied from another board fails
  // here, and so does every record under a header that names no board.
  std::string board_serial;
  uint8_t pga_code;
};

enum class MatchCheck : uint8_t {
  OK,
  WRONG_CHIP,
  WRONG_CHANNEL_COUNT,
  WRONG_BOARD_MODEL,
  NO_BOARD_SERIAL,
  WRONG_BOARD_SERIAL,
  PGA_MISMATCH,
};

// The statuses a user sees. None of them stops metering; they only pick the coefficients.
enum class CalStatus : uint8_t {
  NOT_READ,
  FACTORY,      // sound, and checked on the line
  UNVERIFIED,   // sound, but the station never finished: applied with a warning
  MISSING,      // no header, or no meter.cal: design values
  CORRUPT,      // a CRC or a field that does not hold
  FOREIGN,      // written for another chip, model or board
  UNSUPPORTED,  // a version, signature or PGA plan this firmware does not take
  NO_EEPROM,    // the metering board's EEPROM did not answer
};

inline uint8_t channels_of(ChipType chip) { return chip == ChipType::ATM90E32 ? ATM90E32_CHANNELS : BL0906_CHANNELS; }

inline bool is_known_chip(uint8_t chip_type) {
  return chip_type == static_cast<uint8_t>(ChipType::ATM90E32) || chip_type == static_cast<uint8_t>(ChipType::BL0906);
}

inline bool all_zero(const uint8_t *bytes, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (bytes[i] != 0)
      return false;
  }
  return true;
}

inline RecordCheck check_atm90e32_body(const Atm90e32Body &body, uint16_t flags) {
  if (body.reserved1 != 0 || !all_zero(body.reserved2, sizeof(body.reserved2)))
    return RecordCheck::RESERVED_NOT_ZERO;
  if (body.current_scale != 1 && body.current_scale != 2)
    return RecordCheck::BAD_CURRENT_SCALE;
  for (const Atm90e32Phase &phase : body.phase) {
    if ((phase.phi & 0x7F00) != 0)
      return RecordCheck::BAD_PHI;
    if ((flags & FLAG_GAIN_UI) != 0 && (phase.ugain == 0 || phase.igain == 0))
      return RecordCheck::ZERO_GAIN;
  }
  return RecordCheck::OK;
}

inline RecordCheck check_bl0906_body(const Bl0906Body &body, uint16_t flags) {
  if (!all_zero(body.reserved1, sizeof(body.reserved1)) || !all_zero(body.reserved2, sizeof(body.reserved2)))
    return RecordCheck::RESERVED_NOT_ZERO;
  if ((flags & FLAG_GAIN_UI) != 0 && body.kv_milli == 0)
    return RecordCheck::ZERO_GAIN;
  for (size_t c = 0; c < BL0906_CHANNELS; c++) {
    if ((flags & FLAG_GAIN_UI) != 0 && body.ki_milli[c] == 0)
      return RecordCheck::ZERO_GAIN;
    if ((flags & FLAG_GAIN_ENERGY) != 0 && body.kp_micro[c] == 0)
      return RecordCheck::ZERO_GAIN;
  }
  return RecordCheck::OK;
}

// The record on its own: whether it is a version 1 record, intact and self-consistent. The
// version comes before the size and the CRC, so a longer record of a later version reads as
// unsupported rather than broken.
inline RecordCheck check_record(const uint8_t *raw, size_t size) {
  if (size <= offsetof(MeterCalRecord, record_version))
    return RecordCheck::BAD_SIZE;
  if (memcmp(raw, RECORD_MAGIC, RECORD_MAGIC_SIZE) != 0)
    return RecordCheck::BAD_MAGIC;
  if (raw[offsetof(MeterCalRecord, record_version)] != RECORD_VERSION)
    return RecordCheck::UNSUPPORTED_VERSION;
  if (size != RECORD_SIZE)
    return RecordCheck::BAD_SIZE;
  const auto *rec = reinterpret_cast<const MeterCalRecord *>(raw);
  if (jethome_board_info::compute_crc32(raw, RECORD_CRC_DATA_SIZE) != rec->crc32)
    return RecordCheck::BAD_CRC;
  if (rec->signature_version != SIGNATURE_NONE)
    return RecordCheck::UNSUPPORTED_SIGNATURE;
  if (!is_known_chip(rec->chip_type))
    return RecordCheck::UNKNOWN_CHIP;
  const uint16_t flags = rec->flags;
  if ((flags & ~FLAGS_DEFINED) != 0)
    return RecordCheck::UNKNOWN_FLAGS;
  if (rec->ct_nominal_ratio != CT_NOMINAL_RATIO)
    return RecordCheck::UNSUPPORTED_CT_RATIO;
  if (rec->line_freq_hz != 50 && rec->line_freq_hz != 60)
    return RecordCheck::BAD_LINE_FREQUENCY;
  if (rec->chip_type == static_cast<uint8_t>(ChipType::ATM90E32))
    return check_atm90e32_body(rec->body.atm90e32, flags);
  return check_bl0906_body(rec->body.bl0906, flags);
}

// A sound record against the board the firmware drives.
inline MatchCheck match_record(const MeterCalRecord &rec, const Expectation &expect) {
  if (rec.chip_type != static_cast<uint8_t>(expect.chip))
    return MatchCheck::WRONG_CHIP;
  if (rec.channel_count != channels_of(expect.chip))
    return MatchCheck::WRONG_CHANNEL_COUNT;
  if (jethome_board_info::field_to_string(rec.board_model, sizeof(rec.board_model)) != expect.board_model)
    return MatchCheck::WRONG_BOARD_MODEL;
  // Two empty serials would agree, and say nothing about which board the record is for.
  if (expect.board_serial.empty())
    return MatchCheck::NO_BOARD_SERIAL;
  if (jethome_board_info::field_to_string(rec.board_serial, sizeof(rec.board_serial)) != expect.board_serial)
    return MatchCheck::WRONG_BOARD_SERIAL;
  if (rec.pga_code != expect.pga_code)
    return MatchCheck::PGA_MISMATCH;
  return MatchCheck::OK;
}

// Shipped boards carry a blank part, so no header and no file are the same quiet answer.
inline CalStatus status_of(ReadStatus status) {
  switch (status) {
    case ReadStatus::OK:
      return CalStatus::FACTORY;
    case ReadStatus::NO_EEPROM:
    case ReadStatus::READ_ERROR:
      return CalStatus::NO_EEPROM;
    case ReadStatus::NO_HEADER:
    case ReadStatus::NOT_FOUND:
      return CalStatus::MISSING;
    case ReadStatus::UNSUPPORTED:
    case ReadStatus::TOO_LARGE:
      return CalStatus::UNSUPPORTED;
    case ReadStatus::CORRUPT:
    case ReadStatus::BAD_NAME:
      return CalStatus::CORRUPT;
  }
  return CalStatus::CORRUPT;
}

inline CalStatus status_of(RecordCheck check) {
  switch (check) {
    case RecordCheck::OK:
      return CalStatus::FACTORY;
    case RecordCheck::UNSUPPORTED_VERSION:
    case RecordCheck::UNSUPPORTED_SIGNATURE:
    case RecordCheck::UNKNOWN_FLAGS:
    case RecordCheck::RESERVED_NOT_ZERO:
    case RecordCheck::UNSUPPORTED_CT_RATIO:
      return CalStatus::UNSUPPORTED;
    case RecordCheck::UNKNOWN_CHIP:
      return CalStatus::FOREIGN;
    case RecordCheck::BAD_SIZE:
    case RecordCheck::BAD_MAGIC:
    case RecordCheck::BAD_CRC:
    case RecordCheck::BAD_LINE_FREQUENCY:
    case RecordCheck::BAD_CURRENT_SCALE:
    case RecordCheck::BAD_PHI:
    case RecordCheck::ZERO_GAIN:
      return CalStatus::CORRUPT;
  }
  return CalStatus::CORRUPT;
}

inline CalStatus status_of(MatchCheck check) {
  switch (check) {
    case MatchCheck::OK:
      return CalStatus::FACTORY;
    case MatchCheck::PGA_MISMATCH:
      return CalStatus::UNSUPPORTED;
    case MatchCheck::WRONG_CHIP:
    case MatchCheck::WRONG_CHANNEL_COUNT:
    case MatchCheck::WRONG_BOARD_MODEL:
    case MatchCheck::NO_BOARD_SERIAL:
    case MatchCheck::WRONG_BOARD_SERIAL:
      return CalStatus::FOREIGN;
  }
  return CalStatus::FOREIGN;
}

// The whole verdict: the read, then the record, then the match, then the line check flag.
struct Evaluation {
  CalStatus status{CalStatus::NOT_READ};
  ReadStatus read{ReadStatus::NO_EEPROM};
  RecordCheck record{RecordCheck::OK};
  MatchCheck match{MatchCheck::OK};
};

inline Evaluation evaluate(ReadStatus read, const uint8_t *raw, size_t size, const Expectation &expect) {
  Evaluation result;
  result.read = read;
  if (read != ReadStatus::OK) {
    result.status = status_of(read);
    return result;
  }
  result.record = check_record(raw, size);
  if (result.record != RecordCheck::OK) {
    result.status = status_of(result.record);
    return result;
  }
  const auto *rec = reinterpret_cast<const MeterCalRecord *>(raw);
  result.match = match_record(*rec, expect);
  if (result.match != MatchCheck::OK) {
    result.status = status_of(result.match);
    return result;
  }
  result.status = (rec->flags & FLAG_LINE_VERIFIED) != 0 ? CalStatus::FACTORY : CalStatus::UNVERIFIED;
  return result;
}

// Lower case: the words the status goes by in logs and, later, in the device API.
inline const char *cal_status_str(CalStatus status) {
  switch (status) {
    case CalStatus::NOT_READ:
      return "not_read";
    case CalStatus::FACTORY:
      return "factory";
    case CalStatus::UNVERIFIED:
      return "unverified";
    case CalStatus::MISSING:
      return "missing";
    case CalStatus::CORRUPT:
      return "corrupt";
    case CalStatus::FOREIGN:
      return "foreign";
    case CalStatus::UNSUPPORTED:
      return "unsupported";
    case CalStatus::NO_EEPROM:
      return "no_eeprom";
  }
  return "unknown";
}

inline const char *record_check_str(RecordCheck check) {
  switch (check) {
    case RecordCheck::OK:
      return "sound";
    case RecordCheck::BAD_SIZE:
      return "not 256 bytes";
    case RecordCheck::BAD_MAGIC:
      return "not a calibration record";
    case RecordCheck::UNSUPPORTED_VERSION:
      return "unknown record version";
    case RecordCheck::BAD_CRC:
      return "record CRC32 mismatch";
    case RecordCheck::UNSUPPORTED_SIGNATURE:
      return "unknown signature version";
    case RecordCheck::UNKNOWN_CHIP:
      return "unknown chip type";
    case RecordCheck::UNKNOWN_FLAGS:
      return "unknown flag bits";
    case RecordCheck::RESERVED_NOT_ZERO:
      return "reserved bytes are not zero";
    case RecordCheck::UNSUPPORTED_CT_RATIO:
      return "CT nominal ratio is not 2000";
    case RecordCheck::BAD_LINE_FREQUENCY:
      return "line frequency is neither 50 nor 60 Hz";
    case RecordCheck::BAD_CURRENT_SCALE:
      return "current scale is neither 1 nor 2";
    case RecordCheck::BAD_PHI:
      return "a phase code has bits 14:8 set";
    case RecordCheck::ZERO_GAIN:
      return "a calibrated gain is zero";
  }
  return "unknown";
}

inline const char *match_check_str(MatchCheck check) {
  switch (check) {
    case MatchCheck::OK:
      return "this board's";
    case MatchCheck::WRONG_CHIP:
      return "written for another chip";
    case MatchCheck::WRONG_CHANNEL_COUNT:
      return "written for another channel count";
    case MatchCheck::WRONG_BOARD_MODEL:
      return "written for another board model";
    case MatchCheck::NO_BOARD_SERIAL:
      return "board header has no serial";
    case MatchCheck::WRONG_BOARD_SERIAL:
      return "written for another board serial";
    case MatchCheck::PGA_MISMATCH:
      return "taken at another PGA setting";
  }
  return "unknown";
}

inline const char *chip_name(uint8_t chip_type) {
  switch (chip_type) {
    case static_cast<uint8_t>(ChipType::ATM90E32):
      return "ATM90E32";
    case static_cast<uint8_t>(ChipType::BL0906):
      return "BL0906";
    default:
      return "unknown";
  }
}

}  // namespace esphome::meter_calibration
