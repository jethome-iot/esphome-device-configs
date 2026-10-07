// meter.cal on its own: the layout, every check the record goes through in its order, the
// match against the board the firmware drives, and the status each outcome comes to.

#include <initializer_list>

#include "common.h"

namespace esphome::meter_calibration::testing {
namespace {

const MeterCalRecord &as_record(const MeterRecordBuf &buf) {
  return *reinterpret_cast<const MeterCalRecord *>(buf.data());
}

RecordCheck check(const MeterRecordBuf &buf) { return check_record(buf.data(), buf.size()); }

// Changes one byte and puts the CRC right again, so only the change itself is judged.
MeterRecordBuf with_byte(MeterRecordBuf buf, size_t off, uint8_t value) {
  buf[off] = value;
  seal_meter_record(buf);
  return buf;
}

MeterRecordBuf with_u16(MeterRecordBuf buf, size_t off, uint16_t value) {
  put_u16(buf.data(), off, value);
  seal_meter_record(buf);
  return buf;
}

MeterRecordBuf with_u32(MeterRecordBuf buf, size_t off, uint32_t value) {
  put_u32(buf.data(), off, value);
  seal_meter_record(buf);
  return buf;
}

MeterRecordBuf from(const std::array<uint8_t, RECORD_SIZE> &vector) {
  MeterRecordBuf buf;
  std::copy(vector.begin(), vector.end(), buf.begin());
  return buf;
}

// --- Layout -------------------------------------------------------------------------------

TEST(MeterRecord, TheBuilderWritesThePinnedVectors) {
  EXPECT_EQ(make_meter_record(ChipType::ATM90E32), from(ATM_RECORD_V1));
  EXPECT_EQ(make_meter_record(ChipType::BL0906), from(BL0906_RECORD_V1));
  EXPECT_EQ(get_u32(ATM_RECORD_V1.data(), MC_OFF_CRC32), ATM_RECORD_V1_CRC);
  EXPECT_EQ(get_u32(BL0906_RECORD_V1.data(), MC_OFF_CRC32), BL0906_RECORD_V1_CRC);
}

TEST(MeterRecord, AnAtm90e32RecordReadsFieldByField) {
  const MeterRecordBuf buf = from(ATM_RECORD_V1);
  ASSERT_EQ(check(buf), RecordCheck::OK);
  const MeterCalRecord &rec = as_record(buf);

  EXPECT_EQ(rec.record_version, 1);
  EXPECT_EQ(rec.signature_version, 0);
  EXPECT_EQ(rec.chip_type, 1);
  EXPECT_EQ(rec.channel_count, 3);
  EXPECT_EQ(jethome_board_info::field_to_string(rec.board_model, 32), "JXD-D3-PM3-3");
  EXPECT_EQ(jethome_board_info::field_to_string(rec.board_revision, 8), "1.2");
  EXPECT_EQ(jethome_board_info::field_to_string(rec.board_serial, 32), "PM33-2026-000123");
  EXPECT_EQ(rec.ref_voltage_mv, 230000u);
  EXPECT_EQ(rec.ref_current_ua, 5000u);
  EXPECT_EQ(rec.ct_nominal_ratio, 2000);
  EXPECT_EQ(rec.line_freq_hz, 50);
  EXPECT_EQ(rec.pga_code, 0x15);
  EXPECT_EQ(rec.ambient_ddeg, 235);
  EXPECT_EQ(rec.flags, 0x003F);
  EXPECT_EQ(jethome_board_info::field_to_string(rec.station_id, 12), "ST-01");
  EXPECT_EQ(jethome_board_info::field_to_string(rec.ref_instrument_id, 16), "REF-6305-0042");
  EXPECT_EQ(rec.timestamp, CALIBRATED_AT);
  EXPECT_EQ(rec.operator_code, 7u);
  EXPECT_EQ(rec.crc32, ATM_RECORD_V1_CRC);

  const Atm90e32Body &atm = rec.body.atm90e32;
  EXPECT_EQ(atm.phase[0].ugain, 39470);
  EXPECT_EQ(atm.phase[0].igain, 31600);
  EXPECT_EQ(atm.phase[0].uoffset, -12);
  EXPECT_EQ(atm.phase[0].ioffset, 7);
  EXPECT_EQ(atm.phase[0].poffset, 3);
  EXPECT_EQ(atm.phase[0].qoffset, -4);
  EXPECT_EQ(atm.phase[0].pqgain, -150);
  EXPECT_EQ(atm.phase[0].phi, 0x8012);
  EXPECT_EQ(atm.phase[1].igain, 31702);
  EXPECT_EQ(atm.phase[2].ugain, 39555);
  EXPECT_EQ(atm.phase[2].pqgain, 88);
  EXPECT_EQ(atm.phase[2].phi, 0x0007);
  EXPECT_EQ(atm.current_scale, 2);
  EXPECT_EQ(atm.plconst, 0u);
}

TEST(MeterRecord, ABl0906RecordReadsFieldByField) {
  const MeterRecordBuf buf = from(BL0906_RECORD_V1);
  ASSERT_EQ(check(buf), RecordCheck::OK);
  const MeterCalRecord &rec = as_record(buf);

  EXPECT_EQ(rec.chip_type, 2);
  EXPECT_EQ(rec.channel_count, 6);
  EXPECT_EQ(jethome_board_info::field_to_string(rec.board_model, 32), "JXD-D3-PM1-6");
  EXPECT_EQ(jethome_board_info::field_to_string(rec.board_revision, 8), "1.1.0");
  EXPECT_EQ(rec.pga_code, 0);

  const Bl0906Body &bl = rec.body.bl0906;
  EXPECT_EQ(bl.kv_milli, 14998000u);
  EXPECT_EQ(bl.ki_milli[0], 59856000u);
  EXPECT_EQ(bl.ki_milli[5], 59866000u);
  EXPECT_EQ(bl.kp_micro[2], 213990000u);
  EXPECT_EQ(bl.phase_mdeg[0], 120);
  EXPECT_EQ(bl.phase_mdeg[1], -80);
  EXPECT_EQ(bl.i_off_ma[2], 22);
  EXPECT_EQ(bl.p_off_cw[1], -3);
  EXPECT_EQ(bl.err_worst[5], 49);
}

// --- Checks, in order ---------------------------------------------------------------------

TEST(MeterRecord, ARecordTooShortToCarryAVersionIsTheWrongSize) {
  const MeterRecordBuf buf = make_meter_record();
  EXPECT_EQ(check_record(buf.data(), 0), RecordCheck::BAD_SIZE);
  EXPECT_EQ(check_record(buf.data(), MC_OFF_RECORD_VERSION), RecordCheck::BAD_SIZE);
  EXPECT_EQ(check_record(buf.data(), MC_OFF_RECORD_VERSION + 1), RecordCheck::BAD_SIZE);
  EXPECT_EQ(check_record(buf.data(), RECORD_SIZE - 1), RecordCheck::BAD_SIZE);
}

TEST(MeterRecord, AVersionOneRecordLongerThan256BytesIsTheWrongSize) {
  std::vector<uint8_t> longer(300, 0);
  const MeterRecordBuf buf = make_meter_record();
  std::copy(buf.begin(), buf.end(), longer.begin());
  EXPECT_EQ(check_record(longer.data(), longer.size()), RecordCheck::BAD_SIZE);
}

// All eight bytes count, the two NULs included.
TEST(MeterRecord, EveryMagicByteIsChecked) {
  for (size_t i = 0; i < RECORD_MAGIC_SIZE; i++) {
    SCOPED_TRACE(i);
    MeterRecordBuf buf = make_meter_record();
    buf[MC_OFF_MAGIC + i] ^= 0x20;
    EXPECT_EQ(check(buf), RecordCheck::BAD_MAGIC);
  }
}

// A later version may be longer and keep its CRC elsewhere, so neither is looked at first.
TEST(MeterRecord, AnUnknownVersionIsUnsupportedWhateverItsSizeAndCrc) {
  for (uint8_t version : {0, 2, 0xFF}) {
    SCOPED_TRACE(version);
    MeterRecordBuf buf = make_meter_record();
    buf[MC_OFF_RECORD_VERSION] = version;
    EXPECT_EQ(check(buf), RecordCheck::UNSUPPORTED_VERSION);
    std::vector<uint8_t> longer(512, 0xA5);
    std::copy(buf.begin(), buf.end(), longer.begin());
    EXPECT_EQ(check_record(longer.data(), longer.size()), RecordCheck::UNSUPPORTED_VERSION);
  }
}

TEST(MeterRecord, AChangeToAnyCoveredByteBreaksTheCrc) {
  for (size_t off = MC_OFF_SIG_VERSION; off < RECORD_SIZE; off++) {
    SCOPED_TRACE(off);
    MeterRecordBuf buf = make_meter_record();
    buf[off] ^= 0x01;
    EXPECT_EQ(check(buf), RecordCheck::BAD_CRC);
  }
}

TEST(MeterRecord, ASignatureVersionIsNotYetDefined) {
  for (uint8_t version : {1, 2, 0xFF}) {
    SCOPED_TRACE(version);
    EXPECT_EQ(check(with_byte(make_meter_record(), MC_OFF_SIG_VERSION, version)), RecordCheck::UNSUPPORTED_SIGNATURE);
  }
}

TEST(MeterRecord, AChipTypeBeyondTheTwoIsUnknown) {
  for (uint8_t chip : {0, 3, 0xFF}) {
    SCOPED_TRACE(chip);
    EXPECT_EQ(check(with_byte(make_meter_record(), MC_OFF_CHIP_TYPE, chip)), RecordCheck::UNKNOWN_CHIP);
  }
}

TEST(MeterRecord, FlagBitsVersionOneDoesNotDefineAreRefused) {
  for (int bit = 6; bit < 16; bit++) {
    SCOPED_TRACE(bit);
    EXPECT_EQ(check(with_u16(make_meter_record(), MC_OFF_FLAGS, static_cast<uint16_t>(0x003F | (1u << bit)))),
              RecordCheck::UNKNOWN_FLAGS);
  }
}

TEST(MeterRecord, ReservedBytesMustStayZero) {
  for (size_t off : {ATM_OFF_RESERVED1, ATM_OFF_RESERVED2, ATM_OFF_RESERVED2 + 39}) {
    SCOPED_TRACE(off);
    EXPECT_EQ(check(with_byte(make_meter_record(ChipType::ATM90E32), off, 1)), RecordCheck::RESERVED_NOT_ZERO);
  }
  for (size_t off : {BL_OFF_RESERVED1, BL_OFF_RESERVED1 + 1, BL_OFF_RESERVED2, BL_OFF_RESERVED2 + 9}) {
    SCOPED_TRACE(off);
    EXPECT_EQ(check(with_byte(make_meter_record(ChipType::BL0906), off, 1)), RecordCheck::RESERVED_NOT_ZERO);
  }
}

TEST(MeterRecord, OnlyA2000To1NominalRatioIsTaken) {
  for (uint16_t ratio : {0, 1000, 1999, 2001, 3000}) {
    SCOPED_TRACE(ratio);
    EXPECT_EQ(check(with_u16(make_meter_record(), MC_OFF_CT_RATIO, ratio)), RecordCheck::UNSUPPORTED_CT_RATIO);
  }
}

TEST(MeterRecord, TheLineFrequencyIs50Or60) {
  EXPECT_EQ(check(with_byte(make_meter_record(), MC_OFF_LINE_FREQ, 60)), RecordCheck::OK);
  for (uint8_t hz : {0, 49, 51, 59, 61, 255}) {
    SCOPED_TRACE(hz);
    EXPECT_EQ(check(with_byte(make_meter_record(), MC_OFF_LINE_FREQ, hz)), RecordCheck::BAD_LINE_FREQUENCY);
  }
}

TEST(MeterRecord, TheCurrentScaleIs1Or2) {
  EXPECT_EQ(check(with_byte(make_meter_record(), ATM_OFF_CURRENT_SCALE, 1)), RecordCheck::OK);
  for (uint8_t scale : {0, 3, 4}) {
    SCOPED_TRACE(scale);
    EXPECT_EQ(check(with_byte(make_meter_record(), ATM_OFF_CURRENT_SCALE, scale)), RecordCheck::BAD_CURRENT_SCALE);
  }
}

// Bit 15 picks the direction and bits 7:0 count cycles; the bits between are zero on the chip.
TEST(MeterRecord, APhaseCodeKeepsBits14To8Clear) {
  for (size_t p = 0; p < 3; p++) {
    const size_t off = ATM_OFF_PHASE + p * ATM_PHASE_SIZE + ATM_OFF_PHI;
    EXPECT_EQ(check(with_u16(make_meter_record(), off, 0x80FF)), RecordCheck::OK);
    for (int bit = 8; bit < 15; bit++) {
      SCOPED_TRACE(bit);
      EXPECT_EQ(check(with_u16(make_meter_record(), off, static_cast<uint16_t>(1u << bit))), RecordCheck::BAD_PHI);
    }
  }
}

TEST(MeterRecord, AnAtm90e32GainTheFlagsCallCalibratedIsNotZero) {
  for (size_t p = 0; p < 3; p++) {
    for (size_t field : {ATM_OFF_UGAIN, ATM_OFF_IGAIN}) {
      const size_t off = ATM_OFF_PHASE + p * ATM_PHASE_SIZE + field;
      SCOPED_TRACE(off);
      EXPECT_EQ(check(with_u16(make_meter_record(), off, 0)), RecordCheck::ZERO_GAIN);
      // Without the gain flag nothing claims the gains were calibrated.
      MeterRecordBuf uncalibrated = with_u16(make_meter_record(), off, 0);
      put_u16(uncalibrated.data(), MC_OFF_FLAGS, 0x003F & ~FLAG_GAIN_UI);
      seal_meter_record(uncalibrated);
      EXPECT_EQ(check(uncalibrated), RecordCheck::OK);
    }
  }
}

TEST(MeterRecord, ABl0906CoefficientTheFlagsCallCalibratedIsNotZero) {
  const MeterRecordBuf bl = make_meter_record(ChipType::BL0906);
  EXPECT_EQ(check(with_u32(bl, BL_OFF_KV, 0)), RecordCheck::ZERO_GAIN);
  for (size_t c = 0; c < 6; c++) {
    SCOPED_TRACE(c);
    EXPECT_EQ(check(with_u32(bl, BL_OFF_KI + 4 * c, 0)), RecordCheck::ZERO_GAIN);
    EXPECT_EQ(check(with_u32(bl, BL_OFF_KP + 4 * c, 0)), RecordCheck::ZERO_GAIN);
  }
  // Kv and Ki answer to the gain flag, Kp to the energy flag.
  MeterRecordBuf no_flags = with_u16(bl, MC_OFF_FLAGS, FLAG_LINE_VERIFIED);
  put_u32(no_flags.data(), BL_OFF_KV, 0);
  put_u32(no_flags.data(), BL_OFF_KI, 0);
  put_u32(no_flags.data(), BL_OFF_KP, 0);
  seal_meter_record(no_flags);
  EXPECT_EQ(check(no_flags), RecordCheck::OK);
  MeterRecordBuf gains_only = with_u16(bl, MC_OFF_FLAGS, FLAG_GAIN_UI);
  put_u32(gains_only.data(), BL_OFF_KP, 0);
  seal_meter_record(gains_only);
  EXPECT_EQ(check(gains_only), RecordCheck::OK);
}

// Offsets, PQ gain and the verification results take any value.
TEST(MeterRecord, SignedFieldsTakeTheirWholeRange) {
  MeterRecordBuf buf = make_meter_record();
  for (size_t off : {ATM_OFF_UOFFSET, ATM_OFF_IOFFSET, ATM_OFF_POFFSET, ATM_OFF_QOFFSET, ATM_OFF_PQGAIN})
    put_u16(buf.data(), ATM_OFF_PHASE + off, 0x8000);
  put_u16(buf.data(), ATM_OFF_ERR_PF1, 0x8000);
  put_u16(buf.data(), ATM_OFF_ERR_PF05L + 2, 0x7FFF);
  put_u16(buf.data(), ATM_OFF_ERR_WORST + 4, 0xFFFF);
  put_u32(buf.data(), ATM_OFF_PLCONST, 0x0861C468);
  put_u16(buf.data(), MC_OFF_AMBIENT, 0xFF38);
  seal_meter_record(buf);
  EXPECT_EQ(check(buf), RecordCheck::OK);
  EXPECT_EQ(as_record(buf).ambient_ddeg, -200);
  EXPECT_EQ(as_record(buf).body.atm90e32.phase[0].pqgain, -32768);
}

// The first check that fails is the answer: damage is reported before anything it may have
// caused, and the version before the damage.
TEST(MeterRecord, ChecksRunInTheirOrder) {
  MeterRecordBuf buf = make_meter_record();
  buf[MC_OFF_CHIP_TYPE] = 9;
  EXPECT_EQ(check(buf), RecordCheck::BAD_CRC);
  buf[MC_OFF_RECORD_VERSION] = 2;
  EXPECT_EQ(check(buf), RecordCheck::UNSUPPORTED_VERSION);

  MeterRecordBuf signed_foreign = make_meter_record();
  signed_foreign[MC_OFF_SIG_VERSION] = 1;
  signed_foreign[MC_OFF_CHIP_TYPE] = 9;
  seal_meter_record(signed_foreign);
  EXPECT_EQ(check(signed_foreign), RecordCheck::UNSUPPORTED_SIGNATURE);
}

// One fault each check catches; applied before the record is sealed again.
struct Fault {
  void (*apply)(uint8_t *raw);
  RecordCheck check;
};

const Fault UNKNOWN_CHIP{[](uint8_t *raw) { raw[MC_OFF_CHIP_TYPE] = 7; }, RecordCheck::UNKNOWN_CHIP};
const Fault UNKNOWN_FLAG{[](uint8_t *raw) { put_u16(raw, MC_OFF_FLAGS, 0x00BF); }, RecordCheck::UNKNOWN_FLAGS};
const Fault CT_RATIO{[](uint8_t *raw) { put_u16(raw, MC_OFF_CT_RATIO, 1000); }, RecordCheck::UNSUPPORTED_CT_RATIO};
const Fault LINE_FREQ{[](uint8_t *raw) { raw[MC_OFF_LINE_FREQ] = 55; }, RecordCheck::BAD_LINE_FREQUENCY};
const Fault ATM_RESERVED{[](uint8_t *raw) { raw[ATM_OFF_RESERVED2] = 1; }, RecordCheck::RESERVED_NOT_ZERO};
const Fault CURRENT_SCALE{[](uint8_t *raw) { raw[ATM_OFF_CURRENT_SCALE] = 3; }, RecordCheck::BAD_CURRENT_SCALE};
const Fault PHI_A{[](uint8_t *raw) { put_u16(raw, ATM_OFF_PHASE + ATM_OFF_PHI, 0x0100); }, RecordCheck::BAD_PHI};
const Fault UGAIN_A{[](uint8_t *raw) { put_u16(raw, ATM_OFF_PHASE + ATM_OFF_UGAIN, 0); }, RecordCheck::ZERO_GAIN};
const Fault BL_RESERVED{[](uint8_t *raw) { raw[BL_OFF_RESERVED2] = 1; }, RecordCheck::RESERVED_NOT_ZERO};
const Fault BL_KV{[](uint8_t *raw) { put_u32(raw, BL_OFF_KV, 0); }, RecordCheck::ZERO_GAIN};

MeterRecordBuf faulty(ChipType chip, std::initializer_list<const Fault *> faults) {
  MeterRecordBuf buf = make_meter_record(chip);
  for (const Fault *fault : faults)
    fault->apply(buf.data());
  seal_meter_record(buf);
  return buf;
}

// Past the CRC, a record with two faults is named by the check that comes first, and that picks
// the status: foreign, unsupported and corrupt each win where their check runs earlier.
TEST(MeterRecord, OfTwoFaultsTheEarlierCheckIsTheAnswer) {
  struct Pair {
    const char *name;
    ChipType chip;
    const Fault &first;
    const Fault &second;
    CalStatus status;
  };
  const Pair pairs[] = {
      {"chip before flags", ChipType::ATM90E32, UNKNOWN_CHIP, UNKNOWN_FLAG, CalStatus::FOREIGN},
      {"flags before CT ratio", ChipType::ATM90E32, UNKNOWN_FLAG, CT_RATIO, CalStatus::UNSUPPORTED},
      {"CT ratio before line frequency", ChipType::ATM90E32, CT_RATIO, LINE_FREQ, CalStatus::UNSUPPORTED},
      {"line frequency before reserved", ChipType::ATM90E32, LINE_FREQ, ATM_RESERVED, CalStatus::CORRUPT},
      {"reserved before current scale", ChipType::ATM90E32, ATM_RESERVED, CURRENT_SCALE, CalStatus::UNSUPPORTED},
      {"phi before a zero gain of its phase", ChipType::ATM90E32, PHI_A, UGAIN_A, CalStatus::CORRUPT},
      {"BL0906 reserved before a zero gain", ChipType::BL0906, BL_RESERVED, BL_KV, CalStatus::UNSUPPORTED},
  };
  for (const Pair &p : pairs) {
    SCOPED_TRACE(p.name);
    // Each fault on its own is caught, so the pair really carries both.
    EXPECT_EQ(check(faulty(p.chip, {&p.first})), p.first.check);
    EXPECT_EQ(check(faulty(p.chip, {&p.second})), p.second.check);
    const RecordCheck both = check(faulty(p.chip, {&p.first, &p.second}));
    EXPECT_EQ(both, p.first.check);
    EXPECT_EQ(status_of(both), p.status);
  }
}

// --- Matching the board -------------------------------------------------------------------

TEST(MeterMatch, ARecordForThisBoardMatches) {
  EXPECT_EQ(match_record(as_record(from(ATM_RECORD_V1)), pm380()), MatchCheck::OK);
  EXPECT_EQ(match_record(as_record(from(BL0906_RECORD_V1)), pm220()), MatchCheck::OK);
}

TEST(MeterMatch, TheOtherMetersRecordIsForAnotherChip) {
  EXPECT_EQ(match_record(as_record(from(BL0906_RECORD_V1)), pm380()), MatchCheck::WRONG_CHIP);
  EXPECT_EQ(match_record(as_record(from(ATM_RECORD_V1)), pm220()), MatchCheck::WRONG_CHIP);
}

TEST(MeterMatch, TheChannelCountFollowsTheChip) {
  const MeterRecordBuf atm = with_byte(make_meter_record(), MC_OFF_CHANNEL_COUNT, 6);
  EXPECT_EQ(match_record(as_record(atm), pm380()), MatchCheck::WRONG_CHANNEL_COUNT);
  const MeterRecordBuf bl = with_byte(make_meter_record(ChipType::BL0906), MC_OFF_CHANNEL_COUNT, 3);
  EXPECT_EQ(match_record(as_record(bl), pm220()), MatchCheck::WRONG_CHANNEL_COUNT);
}

TEST(MeterMatch, TheModelIsComparedExactly) {
  Expectation other = pm380();
  other.board_model = "JXD-D3-PM3-4";
  EXPECT_EQ(match_record(as_record(make_meter_record()), other), MatchCheck::WRONG_BOARD_MODEL);
  other.board_model = "jxd-d3-pm3-3";
  EXPECT_EQ(match_record(as_record(make_meter_record()), other), MatchCheck::WRONG_BOARD_MODEL);
  other.board_model = "JXD-D3-PM3";
  EXPECT_EQ(match_record(as_record(make_meter_record()), other), MatchCheck::WRONG_BOARD_MODEL);
}

// A field filled to its last byte has no NUL and still compares whole.
TEST(MeterMatch, AFieldThatFillsItsBytesStillCompares) {
  const std::string serial(32, '7');
  MeterRecordBuf buf = make_meter_record();
  put_field(buf.data(), MC_OFF_BOARD_SERIAL, 32, serial);
  seal_meter_record(buf);
  Expectation expect = pm380();
  expect.board_serial = serial;
  EXPECT_EQ(match_record(as_record(buf), expect), MatchCheck::OK);
}

TEST(MeterMatch, ARecordCopiedFromAnotherBoardHasAnotherSerial) {
  Expectation other = pm380();
  other.board_serial = "PM33-2026-000999";
  EXPECT_EQ(match_record(as_record(make_meter_record()), other), MatchCheck::WRONG_BOARD_SERIAL);
}

MeterRecordBuf with_serial(const char *serial) {
  MeterRecordBuf buf = make_meter_record();
  memset(buf.data() + MC_OFF_BOARD_SERIAL, 0, 32);
  put_field(buf.data(), MC_OFF_BOARD_SERIAL, 32, serial);
  seal_meter_record(buf);
  return buf;
}

// A header with no serial names no board, so no record can be shown to be this board's: not even
// one that has no serial either.
TEST(MeterMatch, AHeaderWithNoSerialMatchesNoRecord) {
  Expectation unnamed = pm380();
  unnamed.board_serial = "";
  EXPECT_EQ(match_record(as_record(with_serial("")), unnamed), MatchCheck::NO_BOARD_SERIAL);
  EXPECT_EQ(match_record(as_record(make_meter_record()), unnamed), MatchCheck::NO_BOARD_SERIAL);
}

TEST(MeterMatch, ARecordWithNoSerialIsAnotherBoardsUnderANamedHeader) {
  EXPECT_EQ(match_record(as_record(with_serial("")), pm380()), MatchCheck::WRONG_BOARD_SERIAL);
}

// The model is checked before the serial, so a record for another model says so first.
TEST(MeterMatch, TheSerialIsCheckedAfterTheModel) {
  Expectation other = pm380();
  other.board_model = "JXD-D3-PM3-4";
  other.board_serial = "";
  EXPECT_EQ(match_record(as_record(make_meter_record()), other), MatchCheck::WRONG_BOARD_MODEL);
  other = pm380();
  other.board_serial = "";
  other.pga_code = 0x2A;
  EXPECT_EQ(match_record(as_record(make_meter_record()), other), MatchCheck::NO_BOARD_SERIAL);
}

TEST(MeterMatch, AnotherPgaPlanIsAMismatch) {
  Expectation other = pm380();
  other.pga_code = 0x2A;
  EXPECT_EQ(match_record(as_record(make_meter_record()), other), MatchCheck::PGA_MISMATCH);
}

// --- Statuses -----------------------------------------------------------------------------

TEST(MeterStatus, EachReadOutcomeComesToItsStatus) {
  EXPECT_EQ(status_of(ReadStatus::NO_EEPROM), CalStatus::NO_EEPROM);
  EXPECT_EQ(status_of(ReadStatus::READ_ERROR), CalStatus::NO_EEPROM);
  EXPECT_EQ(status_of(ReadStatus::NO_HEADER), CalStatus::MISSING);
  EXPECT_EQ(status_of(ReadStatus::NOT_FOUND), CalStatus::MISSING);
  EXPECT_EQ(status_of(ReadStatus::UNSUPPORTED), CalStatus::UNSUPPORTED);
  EXPECT_EQ(status_of(ReadStatus::TOO_LARGE), CalStatus::UNSUPPORTED);
  EXPECT_EQ(status_of(ReadStatus::CORRUPT), CalStatus::CORRUPT);
  EXPECT_EQ(status_of(ReadStatus::BAD_NAME), CalStatus::CORRUPT);
  EXPECT_EQ(status_of(static_cast<ReadStatus>(0xEE)), CalStatus::CORRUPT);
}

// The loader's file name is fixed, so BAD_NAME never reaches it; its words stand ready anyway.
TEST(MeterStatus, EachReadOutcomeHasItsReason) {
  EXPECT_STREQ(read_reason_str(ReadStatus::OK), "read");
  EXPECT_STREQ(read_reason_str(ReadStatus::NO_EEPROM), "the metering board's EEPROM did not answer");
  EXPECT_STREQ(read_reason_str(ReadStatus::READ_ERROR), "the metering board's EEPROM did not answer");
  EXPECT_STREQ(read_reason_str(ReadStatus::NO_HEADER), "the metering board's EEPROM has no board header");
  EXPECT_STREQ(read_reason_str(ReadStatus::NOT_FOUND), "the metering board's EEPROM has no meter.cal");
  EXPECT_STREQ(read_reason_str(ReadStatus::UNSUPPORTED),
               "the metering board's EEPROM uses a header or filesystem version this firmware does not read");
  EXPECT_STREQ(read_reason_str(ReadStatus::TOO_LARGE), "meter.cal is larger than a version 1 record");
  EXPECT_STREQ(read_reason_str(ReadStatus::CORRUPT), "the board header, the file chain or meter.cal fails its CRC");
  EXPECT_STREQ(read_reason_str(ReadStatus::BAD_NAME), "meter.cal is not a name the file chain takes");
  EXPECT_STREQ(read_reason_str(static_cast<ReadStatus>(0xEE)), "unknown");
}

TEST(MeterStatus, EachRecordCheckComesToItsStatus) {
  EXPECT_EQ(status_of(RecordCheck::UNSUPPORTED_VERSION), CalStatus::UNSUPPORTED);
  EXPECT_EQ(status_of(RecordCheck::UNSUPPORTED_SIGNATURE), CalStatus::UNSUPPORTED);
  EXPECT_EQ(status_of(RecordCheck::UNKNOWN_FLAGS), CalStatus::UNSUPPORTED);
  EXPECT_EQ(status_of(RecordCheck::RESERVED_NOT_ZERO), CalStatus::UNSUPPORTED);
  EXPECT_EQ(status_of(RecordCheck::UNSUPPORTED_CT_RATIO), CalStatus::UNSUPPORTED);
  EXPECT_EQ(status_of(RecordCheck::UNKNOWN_CHIP), CalStatus::FOREIGN);
  for (RecordCheck c :
       {RecordCheck::BAD_SIZE, RecordCheck::BAD_MAGIC, RecordCheck::BAD_CRC, RecordCheck::BAD_LINE_FREQUENCY,
        RecordCheck::BAD_CURRENT_SCALE, RecordCheck::BAD_PHI, RecordCheck::ZERO_GAIN, static_cast<RecordCheck>(0xEE)}) {
    EXPECT_EQ(status_of(c), CalStatus::CORRUPT);
  }
}

TEST(MeterStatus, EachMatchOutcomeComesToItsStatus) {
  EXPECT_EQ(status_of(MatchCheck::PGA_MISMATCH), CalStatus::UNSUPPORTED);
  for (MatchCheck c : {MatchCheck::WRONG_CHIP, MatchCheck::WRONG_CHANNEL_COUNT, MatchCheck::WRONG_BOARD_MODEL,
                       MatchCheck::NO_BOARD_SERIAL, MatchCheck::WRONG_BOARD_SERIAL, static_cast<MatchCheck>(0xEE)}) {
    EXPECT_EQ(status_of(c), CalStatus::FOREIGN);
  }
}

TEST(MeterStatus, ASoundRecordIsFactoryOnlyOnceTheLineCheckPassed) {
  const MeterRecordBuf verified = make_meter_record();
  Evaluation e = evaluate(ReadStatus::OK, verified.data(), verified.size(), pm380());
  EXPECT_EQ(e.status, CalStatus::FACTORY);
  EXPECT_EQ(e.read, ReadStatus::OK);
  EXPECT_EQ(e.record, RecordCheck::OK);
  EXPECT_EQ(e.match, MatchCheck::OK);

  const MeterRecordBuf pending = with_u16(make_meter_record(), MC_OFF_FLAGS, 0x003F & ~FLAG_LINE_VERIFIED);
  EXPECT_EQ(evaluate(ReadStatus::OK, pending.data(), pending.size(), pm380()).status, CalStatus::UNVERIFIED);
}

// The record is never looked at when the read failed, and the match never when it is unsound.
TEST(MeterStatus, EvaluationStopsAtTheFirstFailure) {
  Evaluation missing = evaluate(ReadStatus::NOT_FOUND, nullptr, 0, pm380());
  EXPECT_EQ(missing.status, CalStatus::MISSING);
  EXPECT_EQ(missing.read, ReadStatus::NOT_FOUND);

  MeterRecordBuf damaged = make_meter_record(ChipType::BL0906);
  damaged[MC_OFF_TIMESTAMP] ^= 1;
  Evaluation corrupt = evaluate(ReadStatus::OK, damaged.data(), damaged.size(), pm380());
  EXPECT_EQ(corrupt.status, CalStatus::CORRUPT);
  EXPECT_EQ(corrupt.record, RecordCheck::BAD_CRC);
  EXPECT_EQ(corrupt.match, MatchCheck::OK);

  const MeterRecordBuf bl = make_meter_record(ChipType::BL0906);
  Evaluation foreign = evaluate(ReadStatus::OK, bl.data(), bl.size(), pm380());
  EXPECT_EQ(foreign.status, CalStatus::FOREIGN);
  EXPECT_EQ(foreign.match, MatchCheck::WRONG_CHIP);
}

TEST(MeterStatus, EveryValueHasAName) {
  EXPECT_STREQ(cal_status_str(CalStatus::NOT_READ), "not_read");
  EXPECT_STREQ(cal_status_str(CalStatus::FACTORY), "factory");
  EXPECT_STREQ(cal_status_str(CalStatus::UNVERIFIED), "unverified");
  EXPECT_STREQ(cal_status_str(CalStatus::MISSING), "missing");
  EXPECT_STREQ(cal_status_str(CalStatus::CORRUPT), "corrupt");
  EXPECT_STREQ(cal_status_str(CalStatus::FOREIGN), "foreign");
  EXPECT_STREQ(cal_status_str(CalStatus::UNSUPPORTED), "unsupported");
  EXPECT_STREQ(cal_status_str(CalStatus::NO_EEPROM), "no_eeprom");
  EXPECT_STREQ(cal_status_str(static_cast<CalStatus>(0xEE)), "unknown");

  for (int c = 0; c <= static_cast<int>(RecordCheck::ZERO_GAIN); c++)
    EXPECT_STRNE(record_check_str(static_cast<RecordCheck>(c)), "unknown") << c;
  EXPECT_STREQ(record_check_str(static_cast<RecordCheck>(0xEE)), "unknown");
  for (int c = 0; c <= static_cast<int>(MatchCheck::PGA_MISMATCH); c++)
    EXPECT_STRNE(match_check_str(static_cast<MatchCheck>(c)), "unknown") << c;
  EXPECT_STREQ(match_check_str(static_cast<MatchCheck>(0xEE)), "unknown");

  EXPECT_STREQ(chip_name(1), "ATM90E32");
  EXPECT_STREQ(chip_name(2), "BL0906");
  EXPECT_STREQ(chip_name(0), "unknown");
  EXPECT_EQ(channels_of(ChipType::ATM90E32), 3);
  EXPECT_EQ(channels_of(ChipType::BL0906), 6);

  EXPECT_EQ(::testing::PrintToString(CalStatus::FOREIGN), "foreign");
  EXPECT_EQ(::testing::PrintToString(RecordCheck::BAD_CRC), "record CRC32 mismatch");
  EXPECT_EQ(::testing::PrintToString(MatchCheck::PGA_MISMATCH), "taken at another PGA setting");
  EXPECT_STREQ(match_check_str(MatchCheck::NO_BOARD_SERIAL), "board header has no serial");
  EXPECT_EQ(::testing::PrintToString(ReadStatus::NOT_FOUND), "NOT_FOUND");
}

}  // namespace
}  // namespace esphome::meter_calibration::testing
