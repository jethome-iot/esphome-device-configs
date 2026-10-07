// What read_file returns for each way a file can be missing or broken, and how an EEPROM that
// is allowed to be blank reports a header it cannot use: a note when blank, a warning otherwise,
// never a failed component.

#include <cstdint>
#include <utility>

#include "common.h"

namespace esphome::jethome_board_info::testing {
namespace {

const std::vector<uint8_t> SOME_DATA(64, 0x5A);

std::vector<uint8_t> one_file_image(uint8_t fs_version = FS_VERSION_JEEFS_V1) {
  return make_image({FileSpec{"wifi.conf", SOME_DATA}}, 8192, fs_version);
}

ReadStatus read_wifi(Board &board, uint16_t *read = nullptr) {
  uint8_t buf[64];
  return board.info.read_file("wifi.conf", buf, sizeof(buf), read);
}

TEST(ReadFile, AFileTheChainListsIsOk) {
  Board board(one_file_image());
  board.boot();
  uint16_t read = 0;
  EXPECT_EQ(read_wifi(board, &read), ReadStatus::OK);
  EXPECT_EQ(read, SOME_DATA.size());
}

TEST(ReadFile, NoFileAreaIsNotFound) {
  Board board(one_file_image(FS_VERSION_NONE));
  board.boot();
  EXPECT_EQ(read_wifi(board), ReadStatus::NOT_FOUND);
  EXPECT_TRUE(board.log().errors.empty());
}

// Said once at boot, not again by every read that runs into it.
TEST(ReadFile, AnUnknownFilesystemVersionIsUnsupported) {
  Board board(one_file_image(7));
  board.boot();
  EXPECT_EQ(read_wifi(board), ReadStatus::UNSUPPORTED);
  EXPECT_EQ(read_wifi(board), ReadStatus::UNSUPPORTED);
  ASSERT_EQ(board.log().errors.size(), 1u);
  EXPECT_TRUE(board.log().has_error("EEPROM 0x54: Unsupported filesystem version 7, ignoring the file area"));
}

TEST(ReadFile, AnUnknownFilesystemVersionOnAnOptionalPartIsOneWarning) {
  Board board(one_file_image(7));
  board.info.set_required(false);
  board.boot();
  EXPECT_EQ(read_wifi(board), ReadStatus::UNSUPPORTED);
  EXPECT_TRUE(board.log().errors.empty());
  ASSERT_EQ(board.log().warnings.size(), 1u);
  EXPECT_TRUE(board.log().has_warning("Unsupported filesystem version 7, ignoring the file area"));
}

TEST(ReadFile, AFileHeaderWithABadCrcIsCorrupt) {
  std::vector<uint8_t> image = one_file_image();
  image[FILE_HEADER_ADDR + FH_OFF_DATA_SIZE] ^= 0x01;
  Board board(image);
  board.boot();
  EXPECT_EQ(read_wifi(board), ReadStatus::CORRUPT);
  EXPECT_TRUE(board.log().has_error("File header CRC32 mismatch while looking for wifi.conf"));
}

TEST(ReadFile, ABrokenLinkIsCorrupt) {
  std::vector<uint8_t> image =
      make_image({FileSpec{"a.bin", std::vector<uint8_t>(16, 1)}, FileSpec{"wifi.conf", SOME_DATA}});
  put_u16(image.data(), FILE_HEADER_ADDR + FH_OFF_NEXT, 0x0400);
  put_u32(image.data() + FILE_HEADER_ADDR, FH_OFF_HEADER_CRC32,
          compute_crc32(image.data() + FILE_HEADER_ADDR, FILE_HEADER_CRC_DATA_SIZE));
  Board board(image);
  board.boot();
  EXPECT_EQ(read_wifi(board), ReadStatus::CORRUPT);
  EXPECT_TRUE(board.log().has_error("File chain is corrupt while looking for wifi.conf"));
}

TEST(ReadFile, ANameNoFileCanHaveIsRefused) {
  Board board(one_file_image());
  board.boot();
  uint8_t buf[4];
  EXPECT_EQ(board.info.read_file("a-name-far-too-long", buf, sizeof(buf)), ReadStatus::BAD_NAME);
  EXPECT_EQ(board.info.read_file(nullptr, buf, sizeof(buf)), ReadStatus::BAD_NAME);
  EXPECT_TRUE(board.log().has_error("'(null)' is not a name a JEEFS file can have"));
}

// The header was read, then the part stopped answering: on the walk, and on the data.
TEST(ReadFile, APartThatStopsAnsweringIsAReadError) {
  Board walk(one_file_image());
  walk.boot();
  walk.bus.present = false;
  EXPECT_EQ(read_wifi(walk), ReadStatus::READ_ERROR);
  EXPECT_TRUE(walk.log().has_error("Failed to read a file header at 0x0100"));

  Board data(one_file_image());
  data.boot();
  data.bus.fail_after_reads = data.bus.reads + 1;
  EXPECT_EQ(read_wifi(data), ReadStatus::READ_ERROR);
  EXPECT_TRUE(data.log().has_error("Failed to read wifi.conf"));
}

// The chain is read wherever the header says there is one, v3 included.
TEST(ReadFile, AV3HeaderWithAFilesystemHasItsChainRead) {
  std::vector<uint8_t> image = make_image({device_id_file(make_record()), FileSpec{"wifi.conf", SOME_DATA}});
  image[HDR_OFF_VERSION] = HEADER_VERSION_V3;
  reseal_header(image);
  Board board(image);
  board.boot();

  EXPECT_EQ(board.info.get_header_version(), HEADER_VERSION_V3);
  EXPECT_TRUE(board.info.has_device_identity());
  EXPECT_EQ(board.info.get_device_serial(), "DSN-2026-000042");
  EXPECT_EQ(read_wifi(board), ReadStatus::OK);
}

// Each way a device.id can be unusable is named in the log, and the header stands without it.
TEST(ReadFile, AnUnusableDeviceIdIsLoggedAndLeftOut) {
  struct Case {
    const char *name;
    std::vector<uint8_t> data;
    const char *error;
  };
  auto record_with = [](size_t off, uint8_t value, bool reseal) {
    RecordBuf record = make_record();
    record[off] = value;
    if (reseal)
      seal_record(record);
    return std::vector<uint8_t>(record.begin(), record.end());
  };
  const std::vector<Case> cases = {
      {"short", std::vector<uint8_t>(100, 0x5A), "device.id is not 256 bytes"},
      {"magic", record_with(REC_OFF_MAGIC, 'X', true), "Invalid device.id magic"},
      {"version", record_with(REC_OFF_RECORD_VERSION, 9, true), "Unsupported device identity version: 9"},
      {"signature", record_with(REC_OFF_SIG_VERSION, 7, true), "Unsupported device.id signature version: 7"},
      {"record CRC", record_with(REC_OFF_HW_REVISION, 'Z', false), "device.id record CRC32 mismatch"},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.name);
    Board board(make_image({FileSpec{DEVICE_ID_NAME, c.data}}));
    board.boot();
    EXPECT_TRUE(board.info.is_valid());
    EXPECT_FALSE(board.info.has_device_identity());
    EXPECT_TRUE(board.log().has_error(c.error));
  }
}

// A timestamp no calendar holds is reported as such, not formatted.
TEST(ReadFile, ATimestampPastTheCalendarStillDumps) {
  std::vector<uint8_t> image = one_file_image();
  put_u32(image.data(), HDR_OFF_TIMESTAMP, 0xFFFFFFFF);
  put_u32(image.data(), HDR_OFF_TIMESTAMP + 4, 0x7FFFFFFF);
  reseal_header(image);
  Board board(image);
  board.boot();
  EXPECT_TRUE(board.info.is_valid());
  EXPECT_EQ(board.info.get_timestamp(), INT64_MAX);
  board.info.dump_config();
  EXPECT_TRUE(board.log().has_config("Timestamp: invalid (9223372036854775807)"));
  EXPECT_FALSE(board.log().has_config("UTC"));
}

// A part that may be blank: each header it cannot use leaves the component running, says why,
// and hands that reason to every read.
struct OptionalCase {
  const char *name;
  std::vector<uint8_t> image;
  ReadStatus status;
  const char *warning;
};

std::vector<uint8_t> with_bad_crc() {
  std::vector<uint8_t> image = one_file_image();
  image[HDR_OFF_BOARDVERSION] ^= 0x01;
  return image;
}

std::vector<uint8_t> with_version(uint8_t version) {
  std::vector<uint8_t> image = one_file_image();
  image[HDR_OFF_VERSION] = version;
  reseal_header(image);
  return image;
}

// Blank as shipped, zeroed, and erased but for the byte an old test station cleared.
std::vector<uint8_t> erased_but_byte_one() {
  std::vector<uint8_t> image(8192, ERASED_BYTE);
  image[1] = 0x00;
  return image;
}

void expect_running_without_a_header(Board &board, ReadStatus status) {
  EXPECT_FALSE(board.info.is_failed());
  EXPECT_FALSE(board.info.status_has_error());
  EXPECT_FALSE(board.info.is_valid());
  EXPECT_EQ(board.info.get_header_status(), status);
  EXPECT_TRUE(board.log().errors.empty());
  EXPECT_EQ(read_wifi(board), status);
  board.info.dump_config();
  EXPECT_TRUE(board.log().has_config(read_status_str(status)));
}

// Boards shipped before anything was written there: expected, so a note and nothing louder.
TEST(OptionalEeprom, ABlankPartIsANoteNotAWarning) {
  const std::vector<std::pair<const char *, std::vector<uint8_t>>> cases = {
      {"erased", std::vector<uint8_t>(8192, ERASED_BYTE)},
      {"zeroed", std::vector<uint8_t>(8192, EMPTY_BYTE)},
      {"byte one cleared", erased_but_byte_one()},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(c.first);
    Board board(c.second);
    board.info.set_required(false);
    board.boot();

    expect_running_without_a_header(board, ReadStatus::NO_HEADER);
    EXPECT_TRUE(board.log().warnings.empty());
    EXPECT_TRUE(board.log().has_info("EEPROM 0x54: no board header"));
  }
}

TEST(OptionalEeprom, AHeaderThatIsThereButUnusableIsAWarning) {
  const std::vector<OptionalCase> cases = {
      {"bad CRC", with_bad_crc(), ReadStatus::CORRUPT, "EEPROM 0x54: CRC32 mismatch"},
      {"newer header", with_version(5), ReadStatus::UNSUPPORTED, "EEPROM 0x54: Unsupported header version: 5"},
  };
  for (const OptionalCase &c : cases) {
    SCOPED_TRACE(c.name);
    Board board(c.image);
    board.info.set_required(false);
    board.boot();

    expect_running_without_a_header(board, c.status);
    EXPECT_TRUE(board.log().has_warning(c.warning));
    EXPECT_FALSE(board.log().has_info("no board header"));
  }
}

// The same blank part on a board that must carry a header is an error and fails the component.
TEST(OptionalEeprom, ABlankRequiredPartIsStillAnError) {
  Board board(std::vector<uint8_t>(8192, ERASED_BYTE));
  board.boot();

  EXPECT_TRUE(board.info.is_failed());
  EXPECT_TRUE(board.log().has_error("EEPROM 0x54: Invalid magic"));
  EXPECT_FALSE(board.log().has_info("no board header"));
}

TEST(OptionalEeprom, APartThatNeverAnsweredIsNoEeprom) {
  Board board(one_file_image());
  board.bus.present = false;
  board.eeprom.setup();
  LogCapture::instance().clear();
  board.info.set_required(false);
  board.boot();

  EXPECT_FALSE(board.info.is_failed());
  EXPECT_EQ(board.info.get_header_status(), ReadStatus::NO_EEPROM);
  EXPECT_TRUE(board.log().has_warning("EEPROM 0x54: not available"));
  EXPECT_TRUE(board.log().errors.empty());
  EXPECT_EQ(read_wifi(board), ReadStatus::NO_EEPROM);
}

// The bus refuses the header read itself; i2c_eeprom says so as an error of its own.
TEST(OptionalEeprom, AHeaderReadThatFailsIsNoEeprom) {
  Board board(one_file_image());
  board.bus.present = false;
  board.info.set_required(false);
  board.boot();

  EXPECT_FALSE(board.info.is_failed());
  EXPECT_EQ(board.info.get_header_status(), ReadStatus::NO_EEPROM);
  EXPECT_TRUE(board.log().has_warning("Failed to read the 256-byte header"));
}

// A good header on an optional part reads exactly as on a required one; only problems change
// their level.
TEST(OptionalEeprom, ProblemsPastTheHeaderAreWarningsToo) {
  std::vector<uint8_t> image = one_file_image();
  image[FILE_HEADER_ADDR + FILE_HEADER_SIZE] ^= 0xFF;
  Board board(image);
  board.info.set_required(false);
  board.boot();

  EXPECT_TRUE(board.info.is_valid());
  EXPECT_EQ(board.info.get_header_status(), ReadStatus::OK);
  EXPECT_EQ(read_wifi(board), ReadStatus::CORRUPT);
  EXPECT_TRUE(board.log().has_warning("wifi.conf file CRC32 mismatch"));
  EXPECT_TRUE(board.log().errors.empty());
  board.info.dump_config();
}

TEST(OptionalEeprom, IsRequiredUnlessToldOtherwise) {
  Board board(one_file_image());
  EXPECT_TRUE(board.info.is_required());
  board.info.set_required(false);
  EXPECT_FALSE(board.info.is_required());
}

}  // namespace
}  // namespace esphome::jethome_board_info::testing
