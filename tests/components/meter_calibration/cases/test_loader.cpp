// The loader over a metering board's EEPROM: what it reports for each image a board can carry,
// and that none of them fails a component.

#include "common.h"

namespace esphome::meter_calibration::testing {
namespace {

// Whatever the image, metering goes on: nothing is failed and nothing raises a status flag.
void expect_nothing_failed(MeterBoard &board) {
  EXPECT_FALSE(board.info.is_failed());
  EXPECT_FALSE(board.info.status_has_error());
  EXPECT_FALSE(board.cal.is_failed());
  EXPECT_FALSE(board.cal.status_has_error());
  EXPECT_FALSE(board.cal.status_has_warning());
  EXPECT_TRUE(board.log().errors.empty());
}

void expect_no_record(const MeterBoard &board) {
  EXPECT_FALSE(board.cal.has_record());
  EXPECT_FALSE(board.cal.is_verified());
  EXPECT_EQ(board.cal.get_atm90e32(), nullptr);
  EXPECT_EQ(board.cal.get_bl0906(), nullptr);
  EXPECT_EQ(board.cal.get_board_serial(), "");
  EXPECT_EQ(board.cal.get_record_crc32(), 0u);
}

MeterRecordBuf with_flags(uint16_t flags) {
  MeterRecordBuf buf = make_meter_record();
  put_u16(buf.data(), MC_OFF_FLAGS, flags);
  seal_meter_record(buf);
  return buf;
}

TEST(MeterLoader, AFactoryRecordForThisBoardIsFactory) {
  MeterBoard board(meter_image(make_meter_record()));
  board.boot();

  expect_nothing_failed(board);
  EXPECT_TRUE(board.log().warnings.empty());
  EXPECT_EQ(board.cal.get_status(), CalStatus::FACTORY);
  EXPECT_STREQ(board.cal.get_status_str(), "factory");
  EXPECT_EQ(board.cal.get_reason(), "");
  EXPECT_TRUE(board.cal.has_record());
  EXPECT_TRUE(board.cal.is_verified());
  EXPECT_EQ(board.cal.get_read_status(), ReadStatus::OK);
  EXPECT_EQ(board.cal.get_record_check(), RecordCheck::OK);
  EXPECT_EQ(board.cal.get_match_check(), MatchCheck::OK);

  EXPECT_EQ(board.cal.get_board_model(), "JXD-D3-PM3-3");
  EXPECT_EQ(board.cal.get_board_revision(), "1.2");
  EXPECT_EQ(board.cal.get_board_serial(), "PM33-2026-000123");
  EXPECT_EQ(board.cal.get_station_id(), "ST-01");
  EXPECT_EQ(board.cal.get_ref_instrument_id(), "REF-6305-0042");
  EXPECT_EQ(board.cal.get_timestamp(), CALIBRATED_AT);
  EXPECT_EQ(board.cal.get_flags(), 0x003F);
  EXPECT_EQ(board.cal.get_record_crc32(), ATM_RECORD_V1_CRC);

  const Atm90e32Body *atm = board.cal.get_atm90e32();
  ASSERT_NE(atm, nullptr);
  EXPECT_EQ(board.cal.get_bl0906(), nullptr);
  EXPECT_EQ(+atm->phase[0].ugain, 39470);
  EXPECT_EQ(+atm->phase[2].igain, 31511);
  EXPECT_EQ(+atm->current_scale, 2);
  EXPECT_TRUE(board.log().has_info(
      "Factory calibration of JXD-D3-PM3-3 1.2, serial 'PM33-2026-000123', from 2026-10-06 12:00:00 UTC"));
  board.cal.dump_config();
  EXPECT_TRUE(board.log().has_config("Calibrated: 2026-10-06 12:00:00 UTC, station 'ST-01'"));
}

TEST(MeterLoader, ABl0906RecordOnAPm220IsFactory) {
  MeterBoard board(meter_image(make_meter_record(ChipType::BL0906), PM16), ChipType::BL0906);
  board.boot();

  expect_nothing_failed(board);
  EXPECT_EQ(board.cal.get_status(), CalStatus::FACTORY);
  EXPECT_EQ(board.cal.get_atm90e32(), nullptr);
  const Bl0906Body *bl = board.cal.get_bl0906();
  ASSERT_NE(bl, nullptr);
  EXPECT_EQ(+bl->kv_milli, 14998000u);
  EXPECT_EQ(+bl->phase_mdeg[1], -80);
  EXPECT_EQ(board.cal.get_board_serial(), "PM16-2026-000045");
  board.cal.dump_config();
}

// Every byte from the Python builder: the header, meter.cal's file header and the record.
TEST(MeterLoader, AnImageTheFactoryToolWouldWriteIsRead) {
  std::vector<uint8_t> image(8192, 0xFF);
  std::copy(PM33_HEADER_V4.begin(), PM33_HEADER_V4.end(), image.begin());
  std::copy(PM33_METER_CAL_FILE_HEADER.begin(), PM33_METER_CAL_FILE_HEADER.end(), image.begin() + 256);
  std::copy(ATM_RECORD_V1.begin(), ATM_RECORD_V1.end(), image.begin() + 284);
  MeterBoard board(image);
  board.boot();

  expect_nothing_failed(board);
  EXPECT_EQ(board.cal.get_status(), CalStatus::FACTORY);
  EXPECT_EQ(board.info.get_boardname(), "JXD-D3-PM3-3");
  EXPECT_EQ(board.info.get_board_serial(), "PM33-2026-000123");
  EXPECT_EQ(board.info.get_timestamp(), 1791287000);
  EXPECT_EQ(get_u32(PM33_HEADER_V4.data(), HDR_OFF_CRC32), PM33_HEADER_V4_CRC);
}

TEST(MeterLoader, ARecordTheStationNeverFinishedIsUnverified) {
  MeterBoard board(meter_image(with_flags(0x003F & ~FLAG_LINE_VERIFIED)));
  board.boot();

  expect_nothing_failed(board);
  EXPECT_EQ(board.cal.get_status(), CalStatus::UNVERIFIED);
  EXPECT_TRUE(board.cal.has_record());
  EXPECT_FALSE(board.cal.is_verified());
  ASSERT_NE(board.cal.get_atm90e32(), nullptr);
  EXPECT_TRUE(board.log().has_warning("Factory calibration unverified: the station did not finish"));
  board.cal.dump_config();
}

// Shipped boards: blank, zeroed, or erased but for byte 1, which an old test station cleared.
TEST(MeterLoader, ABlankPartIsMissingAndFailsNothing) {
  std::vector<uint8_t> byte_one(8192, 0xFF);
  byte_one[1] = 0x00;
  for (const auto &image : {std::vector<uint8_t>(8192, 0xFF), std::vector<uint8_t>(8192, 0x00), byte_one}) {
    MeterBoard board(image);
    board.boot();

    expect_nothing_failed(board);
    EXPECT_EQ(board.info.get_header_status(), ReadStatus::NO_HEADER);
    EXPECT_EQ(board.cal.get_status(), CalStatus::MISSING);
    EXPECT_EQ(board.cal.get_read_status(), ReadStatus::NO_HEADER);
    EXPECT_EQ(board.cal.get_reason(), "the metering board's EEPROM has no board header");
    expect_no_record(board);
    // A note from each and nothing louder: no record is not a fault.
    EXPECT_TRUE(board.log().warnings.empty());
    EXPECT_TRUE(board.log().has_info("EEPROM 0x56: no board header"));
    EXPECT_TRUE(board.log().has_info("No factory calibration: the metering board's EEPROM has no board header"));
    board.cal.dump_config();
  }
}

TEST(MeterLoader, AHeaderWithoutMeterCalIsMissing) {
  MeterBoard board(meter_image(std::vector<FileSpec>{}));
  board.boot();

  expect_nothing_failed(board);
  EXPECT_TRUE(board.log().warnings.empty());
  EXPECT_EQ(board.cal.get_status(), CalStatus::MISSING);
  EXPECT_EQ(board.cal.get_read_status(), ReadStatus::NOT_FOUND);
  EXPECT_EQ(board.cal.get_reason(), "the metering board's EEPROM has no meter.cal");
  expect_no_record(board);
}

TEST(MeterLoader, AHeaderWithNoFileAreaIsMissing) {
  std::vector<uint8_t> image = meter_image(make_meter_record());
  image[HDR_OFF_FS_VERSION] = jethome_board_info::FS_VERSION_NONE;
  reseal_header(image);
  MeterBoard board(image);
  board.boot();

  expect_nothing_failed(board);
  EXPECT_EQ(board.cal.get_status(), CalStatus::MISSING);
}

TEST(MeterLoader, OtherFilesInTheChainAreWalkedPast) {
  MeterBoard board(
      meter_image({FileSpec{"notes.txt", std::vector<uint8_t>(40, 'x')}, meter_cal_file(make_meter_record())}));
  board.boot();

  expect_nothing_failed(board);
  EXPECT_EQ(board.cal.get_status(), CalStatus::FACTORY);
}

struct BrokenCase {
  const char *name;
  std::vector<uint8_t> image;
  CalStatus status;
  const char *reason;
};

std::vector<uint8_t> flipped(std::vector<uint8_t> image, size_t at) {
  image[at] ^= 0x01;
  return image;
}

// A record whose own CRC is wrong while the file around it was sealed over the bad bytes.
std::vector<uint8_t> inner_crc_broken() {
  MeterRecordBuf rec = make_meter_record();
  rec[MC_OFF_CRC32] ^= 0xFF;
  return meter_image(rec);
}

std::vector<uint8_t> resized(size_t size) {
  MeterRecordBuf rec = make_meter_record();
  std::vector<uint8_t> data(size, 0);
  std::copy(rec.begin(), rec.begin() + std::min(size, rec.size()), data.begin());
  return meter_image({FileSpec{RECORD_FILE_NAME, data}});
}

std::vector<uint8_t> header_version(uint8_t version) {
  std::vector<uint8_t> image = meter_image(make_meter_record());
  image[HDR_OFF_VERSION] = version;
  reseal_header(image);
  return image;
}

std::vector<uint8_t> header_fs_version(uint8_t fs_version) {
  std::vector<uint8_t> image = meter_image(make_meter_record());
  image[HDR_OFF_FS_VERSION] = fs_version;
  reseal_header(image);
  return image;
}

std::vector<uint8_t> record_with(size_t off, uint8_t value) {
  MeterRecordBuf rec = make_meter_record();
  rec[off] = value;
  seal_meter_record(rec);
  return meter_image(rec);
}

std::vector<uint8_t> on_board(const BoardIdentity &board) { return meter_image(make_meter_record(), board); }

const BoardIdentity OTHER_SERIAL{"JXD-D3-PM3-3", "1.2", "PM33-2026-000999"};

TEST(MeterLoader, EveryWayARecordCanBeUnusableHasItsStatusAndReason) {
  const std::vector<uint8_t> good = meter_image(make_meter_record());
  const std::vector<BrokenCase> cases = {
      {"header CRC", flipped(good, HDR_OFF_BOARDVERSION), CalStatus::CORRUPT,
       "the board header, the file chain or meter.cal fails its CRC"},
      {"file header CRC", flipped(good, 256 + 16), CalStatus::CORRUPT,
       "the board header, the file chain or meter.cal fails its CRC"},
      {"file data CRC", flipped(good, 284 + ATM_OFF_PHASE), CalStatus::CORRUPT,
       "the board header, the file chain or meter.cal fails its CRC"},
      {"record CRC", inner_crc_broken(), CalStatus::CORRUPT, "record CRC32 mismatch"},
      {"short file", resized(100), CalStatus::CORRUPT, "not 256 bytes"},
      {"not a record", record_with(MC_OFF_MAGIC, 'X'), CalStatus::CORRUPT, "not a calibration record"},
      {"bad frequency", record_with(MC_OFF_LINE_FREQ, 55), CalStatus::CORRUPT,
       "line frequency is neither 50 nor 60 Hz"},
      {"larger file", resized(300), CalStatus::UNSUPPORTED, "meter.cal is larger than a version 1 record"},
      {"record version", record_with(MC_OFF_RECORD_VERSION, 2), CalStatus::UNSUPPORTED, "unknown record version"},
      {"signature", record_with(MC_OFF_SIG_VERSION, 1), CalStatus::UNSUPPORTED, "unknown signature version"},
      {"header version", header_version(5), CalStatus::UNSUPPORTED,
       "the metering board's EEPROM uses a header or filesystem version this firmware does not read"},
      {"fs version", header_fs_version(2), CalStatus::UNSUPPORTED,
       "the metering board's EEPROM uses a header or filesystem version this firmware does not read"},
      {"PGA", record_with(MC_OFF_PGA, 0x2A), CalStatus::UNSUPPORTED,
       "taken at another PGA setting (record 0x2A, firmware 0x15)"},
      {"unknown chip", record_with(MC_OFF_CHIP_TYPE, 7), CalStatus::FOREIGN, "unknown chip type"},
      {"serial", on_board(OTHER_SERIAL), CalStatus::FOREIGN,
       "written for another board serial (record 'PM33-2026-000123', board header 'PM33-2026-000999')"},
      {"channels", record_with(MC_OFF_CHANNEL_COUNT, 6), CalStatus::FOREIGN,
       "written for another channel count (record 6, firmware 3)"},
      {"chip", meter_image(make_meter_record(ChipType::BL0906)), CalStatus::FOREIGN,
       "written for another chip (record BL0906, firmware ATM90E32)"},
  };
  for (const BrokenCase &c : cases) {
    SCOPED_TRACE(c.name);
    MeterBoard board(c.image);
    board.boot();

    expect_nothing_failed(board);
    EXPECT_EQ(board.cal.get_status(), c.status);
    EXPECT_EQ(board.cal.get_reason(), c.reason);
    expect_no_record(board);
    const std::string warning = std::string("Factory calibration ") + cal_status_str(c.status) + ": " + c.reason;
    EXPECT_TRUE(board.log().has_warning(warning.c_str()));
    board.cal.dump_config();
  }
}

// The board info says it once, the loader once: nothing repeats per file read.
TEST(MeterLoader, AnUnknownFilesystemVersionIsOneWarningFromEach) {
  MeterBoard board(header_fs_version(2));
  board.boot();

  expect_nothing_failed(board);
  EXPECT_EQ(board.cal.get_status(), CalStatus::UNSUPPORTED);
  ASSERT_EQ(board.log().warnings.size(), 2u);
  EXPECT_TRUE(board.log().has_warning("EEPROM 0x56: Unsupported filesystem version 2, ignoring the file area"));
  EXPECT_TRUE(board.log().has_warning("Factory calibration unsupported: "));
}

MeterRecordBuf record_with_serial(const char *serial) {
  MeterRecordBuf rec = make_meter_record();
  memset(rec.data() + MC_OFF_BOARD_SERIAL, 0, 32);
  put_field(rec.data(), MC_OFF_BOARD_SERIAL, 32, serial);
  seal_meter_record(rec);
  return rec;
}

const BoardIdentity NO_SERIAL{"JXD-D3-PM3-3", "1.2", ""};

// Serials are strict: a header that names no board vouches for no record, an empty one included.
TEST(MeterLoader, EverySerialThatDoesNotNameThisBoardIsForeign) {
  struct SerialCase {
    const char *name;
    std::vector<uint8_t> image;
    const char *reason;
  };
  const std::vector<SerialCase> cases = {
      {"header and record empty", meter_image(record_with_serial(""), NO_SERIAL), "board header has no serial"},
      {"header empty", meter_image(make_meter_record(), NO_SERIAL), "board header has no serial"},
      {"record empty", meter_image(record_with_serial("")),
       "written for another board serial (record '', board header 'PM33-2026-000123')"},
  };
  for (const SerialCase &c : cases) {
    SCOPED_TRACE(c.name);
    MeterBoard board(c.image);
    board.boot();

    expect_nothing_failed(board);
    EXPECT_EQ(board.cal.get_status(), CalStatus::FOREIGN);
    EXPECT_EQ(board.cal.get_reason(), c.reason);
    expect_no_record(board);
    const std::string warning = std::string("Factory calibration foreign: ") + c.reason;
    EXPECT_TRUE(board.log().has_warning(warning.c_str()));
  }
}

// v3 keeps the device's serial where v4 keeps the board's: nothing to match a record against,
// so the record is not even read.
TEST(MeterLoader, AV3BoardHeaderIsUnsupported) {
  for (bool with_record : {true, false}) {
    SCOPED_TRACE(with_record);
    std::vector<uint8_t> image = with_record ? meter_image(make_meter_record()) : meter_image(std::vector<FileSpec>{});
    image[HDR_OFF_VERSION] = jethome_board_info::HEADER_VERSION_V3;
    reseal_header(image);
    MeterBoard board(image);
    board.info.setup();
    const int reads = board.bus.reads;
    board.cal.setup();

    EXPECT_EQ(board.bus.reads, reads);
    expect_nothing_failed(board);
    EXPECT_TRUE(board.info.is_valid());
    EXPECT_EQ(board.info.get_header_version(), jethome_board_info::HEADER_VERSION_V3);
    EXPECT_EQ(board.cal.get_status(), CalStatus::UNSUPPORTED);
    EXPECT_EQ(board.cal.get_read_status(), ReadStatus::UNSUPPORTED);
    EXPECT_EQ(board.cal.get_reason(), "board header is not v4");
    expect_no_record(board);
    EXPECT_TRUE(board.log().has_warning("Factory calibration unsupported: board header is not v4"));
    board.cal.dump_config();
  }
}

// A date no calendar holds is said to be invalid, not formatted, and the record stays usable.
TEST(MeterLoader, ATimestampPastTheCalendarStillDumps) {
  MeterRecordBuf rec = make_meter_record();
  put_i64(rec.data(), MC_OFF_TIMESTAMP, INT64_MAX);
  seal_meter_record(rec);
  MeterBoard board(meter_image(rec));
  board.boot();
  EXPECT_EQ(board.cal.get_status(), CalStatus::FACTORY);
  EXPECT_EQ(board.cal.get_timestamp(), INT64_MAX);
  EXPECT_TRUE(board.log().has_info("Factory calibration of JXD-D3-PM3-3 1.2, serial 'PM33-2026-000123', from invalid"));
  board.cal.dump_config();
  EXPECT_TRUE(board.log().has_config("Calibrated: invalid, station 'ST-01'"));
  EXPECT_FALSE(board.log().has_config("UTC"));
}

TEST(MeterLoader, ABl0906RecordForAnotherChannelCountIsForeign) {
  MeterRecordBuf rec = make_meter_record(ChipType::BL0906);
  rec[MC_OFF_CHANNEL_COUNT] = 3;
  seal_meter_record(rec);
  MeterBoard board(meter_image(rec, PM16), ChipType::BL0906);
  board.boot();

  expect_nothing_failed(board);
  EXPECT_EQ(board.cal.get_status(), CalStatus::FOREIGN);
  EXPECT_EQ(board.cal.get_match_check(), MatchCheck::WRONG_CHANNEL_COUNT);
  EXPECT_EQ(board.cal.get_reason(), "written for another channel count (record 3, firmware 6)");
  expect_no_record(board);
}

TEST(MeterLoader, ARecordForAnotherModelIsForeign) {
  MeterBoard board(meter_image(make_meter_record()));
  board.cal.set_board_model("JXD-D3-PM3-4");
  board.boot();

  expect_nothing_failed(board);
  EXPECT_EQ(board.cal.get_status(), CalStatus::FOREIGN);
  EXPECT_EQ(board.cal.get_reason(), "written for another board model (record 'JXD-D3-PM3-3', firmware 'JXD-D3-PM3-4')");
}

TEST(MeterLoader, APartThatNeverAnsweredIsNoEeprom) {
  MeterBoard board(meter_image(make_meter_record()));
  board.bus.present = false;
  board.eeprom.setup();
  ASSERT_TRUE(board.eeprom.is_failed());
  LogCapture::instance().clear();
  board.boot();

  EXPECT_FALSE(board.info.is_failed());
  EXPECT_FALSE(board.cal.is_failed());
  EXPECT_EQ(board.cal.get_status(), CalStatus::NO_EEPROM);
  EXPECT_EQ(board.cal.get_reason(), "the metering board's EEPROM did not answer");
  EXPECT_TRUE(board.log().has_warning("Factory calibration no_eeprom: the metering board's EEPROM did not answer"));
  expect_no_record(board);
}

// The header came back, then the part went quiet before the record was read.
TEST(MeterLoader, APartThatStopsAnsweringIsNoEeprom) {
  MeterBoard board(meter_image(make_meter_record()));
  board.info.setup();
  board.bus.fail_after_reads = board.bus.reads + 1;
  board.cal.setup();

  EXPECT_EQ(board.cal.get_status(), CalStatus::NO_EEPROM);
  EXPECT_EQ(board.cal.get_read_status(), ReadStatus::READ_ERROR);
  expect_no_record(board);
}

TEST(MeterLoader, WithoutABoardInfoThereIsNoEeprom) {
  MeterCalibration cal;
  cal.set_board_model("JXD-D3-PM3-3");
  cal.setup();
  EXPECT_EQ(cal.get_status(), CalStatus::NO_EEPROM);
  EXPECT_FALSE(cal.has_record());
  cal.dump_config();
}

TEST(MeterLoader, NothingIsReadBeforeSetup) {
  MeterBoard board(meter_image(make_meter_record()));
  EXPECT_EQ(board.cal.get_status(), CalStatus::NOT_READ);
  EXPECT_STREQ(board.cal.get_status_str(), "not_read");
  EXPECT_FALSE(board.cal.has_record());
  EXPECT_EQ(board.bus.reads, 0);
}

// The board info has read the header by the time the loader asks for the file.
TEST(MeterLoader, SetsUpAfterTheBoardInfo) {
  MeterBoard board(meter_image(make_meter_record()));
  EXPECT_LT(board.cal.get_setup_priority(), board.info.get_setup_priority());
  EXPECT_GT(board.cal.get_setup_priority(), setup_priority::DATA - 2.0f);
}

}  // namespace
}  // namespace esphome::meter_calibration::testing
