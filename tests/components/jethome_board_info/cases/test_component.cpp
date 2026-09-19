// The component over an EEPROM image: what it publishes for a v4 and a v3 board, which
// signature it hands out, and when it marks itself failed.

#include "common.h"

namespace esphome::jethome_board_info::testing {
namespace {

TEST(BoardInfo, AV4BoardPublishesItsHeaderAndItsRecord) {
  Board board(v4_image(signed_record(0xAB)));
  board.boot();

  EXPECT_FALSE(board.info.is_failed());
  EXPECT_TRUE(board.info.is_valid());
  EXPECT_TRUE(board.info.is_crc_valid());
  EXPECT_EQ(board.info.get_header_version(), HEADER_VERSION_V4);
  EXPECT_EQ(board.info.get_boardname(), "JetHub-D2");
  EXPECT_EQ(board.info.get_boardversion(), "2.1");
  EXPECT_EQ(board.info.get_board_serial(), "BS-2026-00001");
  EXPECT_EQ(board.info.get_usid(), "JXD6E101002601012026000042ABCD");
  EXPECT_EQ(board.info.get_cpuid(), "F0:57:8D:10:20:30");
  EXPECT_EQ(board.info.get_mac_str(), "F0:57:8D:10:20:30");
  EXPECT_EQ(board.info.get_timestamp(), 0);
  ASSERT_TRUE(board.info.has_device_identity());
  EXPECT_EQ(board.info.get_device_model(), "JetHub-D2");
  EXPECT_EQ(board.info.get_hw_revision(), "1.2a");
  EXPECT_EQ(board.info.get_device_serial(), "DSN-2026-000042");
  EXPECT_TRUE(board.info.has_serial_check());
  EXPECT_TRUE(board.info.serial_matches_usid());
  EXPECT_TRUE(board.log().errors.empty());
  EXPECT_TRUE(board.log().warnings.empty());
  // The header, the one file header and the record: nothing is read twice.
  EXPECT_EQ(board.bus.reads, 3);
}

// The signature handed out is the record's, never the header's, once there is a record.
TEST(BoardInfo, TheRecordsSignatureIsTheOneHandedOut) {
  Board board(v4_image(signed_record(0xAB), SIG_CURRENT));
  board.boot();

  EXPECT_EQ(board.info.get_signature_version(), SIG_CURRENT);
  EXPECT_EQ(board.signature(), std::vector<uint8_t>(SIGNATURE_SIZE, 0xAB));
}

TEST(BoardInfo, ARecordWithoutASignatureIsNotCoveredByTheHeaders) {
  Board board(v4_image(make_record(), SIG_CURRENT));
  board.boot();

  EXPECT_TRUE(board.info.has_device_identity());
  EXPECT_EQ(board.info.get_signature_version(), SIG_NONE);
  EXPECT_EQ(board.signature(), std::vector<uint8_t>(SIGNATURE_SIZE, 0));
}

TEST(BoardInfo, AV3BoardTakesTheSerialAndTheSignatureFromTheHeader) {
  Board board(v3_image());
  board.boot();

  EXPECT_TRUE(board.info.is_valid());
  EXPECT_EQ(board.info.get_header_version(), HEADER_VERSION_V3);
  EXPECT_EQ(board.info.get_boardname(), "JXD-CPU-E1ETH");
  EXPECT_EQ(board.info.get_boardversion(), "1.3");
  EXPECT_EQ(board.info.get_device_serial(), "SN-2024-001");
  EXPECT_EQ(board.info.get_board_serial(), "");
  EXPECT_EQ(board.info.get_usid(), "1234567890ABCDEF");
  EXPECT_EQ(board.info.get_cpuid(), "AA:BB:CC:DD:EE:FF");
  EXPECT_EQ(board.info.get_mac_str(), "F0:57:8D:AA:BB:CC");
  EXPECT_FALSE(board.info.has_device_identity());
  EXPECT_FALSE(board.info.has_serial_check());
  EXPECT_EQ(board.info.get_device_model(), "");
  EXPECT_EQ(board.info.get_hw_revision(), "");

  EXPECT_EQ(board.info.get_signature_version(), SIG_CURRENT);
  std::vector<uint8_t> expected(SIGNATURE_SIZE);
  for (size_t i = 0; i < expected.size(); i++)
    expected[i] = static_cast<uint8_t>(i + 1);
  EXPECT_EQ(board.signature(), expected);
  EXPECT_EQ(board.bus.reads, 1);
}

// The version is reported as written, known or not.
TEST(BoardInfo, AnUnknownSignatureVersionIsReportedAsIs) {
  std::vector<uint8_t> image = v3_image();
  image[HDR_OFF_SIG_VERSION] = 3;
  reseal_header(image);
  Board board(image);
  board.boot();

  EXPECT_TRUE(board.info.is_valid());
  EXPECT_EQ(board.info.get_signature_version(), 3);
}

// A v4 board without device.id: what the page falls back to is the board serial.
TEST(BoardInfo, AV4BoardWithoutARecordHasOnlyItsBoardSerial) {
  Board board(make_image({}));
  board.boot();

  EXPECT_TRUE(board.info.is_valid());
  EXPECT_FALSE(board.info.has_device_identity());
  EXPECT_EQ(board.info.get_board_serial(), "BS-2026-00001");
  EXPECT_EQ(board.info.get_device_serial(), "");
  EXPECT_EQ(board.info.get_signature_version(), SIG_NONE);
  EXPECT_TRUE(board.log().errors.empty());
  EXPECT_TRUE(board.log().warnings.empty());
}

TEST(BoardInfo, AnEepromThatDoesNotAnswerMarksTheComponentFailed) {
  Board board(v3_image());
  board.bus.present = false;
  board.boot();

  EXPECT_TRUE(board.info.is_failed());
  EXPECT_FALSE(board.info.is_valid());
  EXPECT_EQ(board.info.get_boardname(), "");
  EXPECT_EQ(board.info.get_signature_version(), SIG_NONE);
  EXPECT_TRUE(board.log().has_error("Failed to read 256 bytes from EEPROM"));
}

// The EEPROM component already found no chip: nothing more goes out on the bus.
TEST(BoardInfo, AFailedEepromComponentIsNotAskedAgain) {
  Board board(v3_image());
  board.bus.present = false;
  board.eeprom.setup();
  ASSERT_TRUE(board.eeprom.is_failed());
  LogCapture::instance().clear();
  board.boot();

  EXPECT_TRUE(board.info.is_failed());
  EXPECT_TRUE(board.log().has_error("EEPROM not available"));
  EXPECT_FALSE(board.log().has_error("Failed to read"));
  EXPECT_EQ(board.bus.reads, 0);
}

TEST(BoardInfo, ACorruptHeaderMarksTheComponentFailed) {
  std::vector<uint8_t> image = v3_image();
  image[HDR_OFF_BOARDVERSION] ^= 0x01;
  Board board(image);
  board.boot();

  EXPECT_TRUE(board.info.is_failed());
  EXPECT_FALSE(board.info.is_valid());
  EXPECT_FALSE(board.info.is_crc_valid());
  EXPECT_TRUE(board.log().has_error("CRC32 mismatch"));
}

TEST(BoardInfo, AForeignEepromMarksTheComponentFailed) {
  Board board(std::vector<uint8_t>(8192, 0xFF));
  board.boot();

  EXPECT_TRUE(board.info.is_failed());
  EXPECT_TRUE(board.log().has_error("Invalid magic"));
}

TEST(BoardInfo, AnOlderHeaderVersionMarksTheComponentFailed) {
  std::vector<uint8_t> image(8192, 0);
  memcpy(image.data(), V2_HEADER_MINIMAL.data(), V2_HEADER_MINIMAL.size());
  Board board(image);
  board.boot();

  EXPECT_TRUE(board.info.is_failed());
  EXPECT_TRUE(board.log().has_error("Unsupported header version: 2"));
}

TEST(BoardInfo, AnUnknownFilesystemVersionKeepsTheHeaderAndSkipsTheFiles) {
  std::vector<uint8_t> image = v4_image(signed_record(0xAB));
  image[HDR_OFF_FS_VERSION] = 7;
  reseal_header(image);
  Board board(image);
  board.boot();

  EXPECT_TRUE(board.info.is_valid());
  EXPECT_FALSE(board.info.has_device_identity());
  EXPECT_TRUE(board.log().has_error("Unsupported filesystem version 7"));
  EXPECT_EQ(board.bus.reads, 1);
}

TEST(BoardInfo, ACorruptRecordIsNotAnIdentity) {
  std::vector<uint8_t> image = v4_image(signed_record(0xAB), SIG_CURRENT);
  image[DEVICE_ID_ADDR + REC_OFF_HW_REVISION] ^= 0x01;
  Board board(image);
  board.boot();

  EXPECT_TRUE(board.info.is_valid());
  EXPECT_FALSE(board.info.has_device_identity());
  EXPECT_EQ(board.info.get_device_serial(), "");
  EXPECT_TRUE(board.log().has_error("device.id file CRC32 mismatch"));
  // Back to the header's signature, whatever it claims.
  EXPECT_EQ(board.info.get_signature_version(), SIG_CURRENT);
  EXPECT_EQ(board.signature(), std::vector<uint8_t>(SIGNATURE_SIZE, 0xCD));
}

// A bus that dies after the header: the header stands, the record does not.
TEST(BoardInfo, ABusErrorInTheChainLeavesTheHeaderWithoutARecord) {
  Board walk(v4_image(signed_record(0xAB)));
  walk.bus.fail_after_reads = 1;
  walk.boot();
  EXPECT_TRUE(walk.info.is_valid());
  EXPECT_FALSE(walk.info.has_device_identity());
  EXPECT_TRUE(walk.log().has_error("Failed to read a file header at 0x0100"));

  Board record(v4_image(signed_record(0xAB)));
  record.bus.fail_after_reads = 2;
  record.boot();
  EXPECT_TRUE(record.info.is_valid());
  EXPECT_FALSE(record.info.has_device_identity());
  EXPECT_TRUE(record.log().has_error("Failed to read device.id"));
}

// An empty record field is not an answer: the serial stays what the header supplied.
TEST(BoardInfo, AnEmptyRecordSerialIsNoSerial) {
  RecordBuf record = make_record();
  memset(record.data() + REC_OFF_DEVICE_SERIAL, 0, 32);
  seal_record(record);
  Board board(v4_image(record));
  board.boot();

  EXPECT_TRUE(board.info.has_device_identity());
  EXPECT_EQ(board.info.get_device_serial(), "");
  EXPECT_FALSE(board.info.has_serial_check());
}

TEST(BoardInfo, ARecordSerialTheUsidDoesNotEncodeIsReported) {
  RecordBuf record = signed_record(0xAB);
  put_field(record.data(), REC_OFF_DEVICE_SERIAL, 32, "DSN-2026-000043");
  seal_record(record);
  Board board(v4_image(record));
  board.boot();

  EXPECT_EQ(board.info.get_device_serial(), "DSN-2026-000043");
  EXPECT_TRUE(board.info.has_serial_check());
  EXPECT_FALSE(board.info.serial_matches_usid());
  EXPECT_TRUE(board.log().has_warning("serial 'DSN-2026-000043' disagrees with the USID"));
}

TEST(BoardInfo, ReadFileReturnsAFileAndChecksItsCrc) {
  const std::vector<uint8_t> data(64, 0x5A);
  Board board(make_image({FileSpec{"wifi.conf", data}, device_id_file(make_record())}));
  board.boot();
  EXPECT_TRUE(board.info.has_device_identity());

  uint8_t buf[64];
  uint16_t read = 0;
  ASSERT_TRUE(board.info.read_file("wifi.conf", buf, sizeof(buf), &read));
  EXPECT_EQ(read, 64);
  EXPECT_EQ(memcmp(buf, data.data(), data.size()), 0);

  uint8_t small[10];
  EXPECT_FALSE(board.info.read_file("wifi.conf", small, sizeof(small)));
  EXPECT_TRUE(board.log().has_error("wifi.conf is 64 bytes, the buffer holds 10"));

  EXPECT_FALSE(board.info.read_file("nothing", buf, sizeof(buf)));

  board.bus.image[FILE_HEADER_ADDR + FILE_HEADER_SIZE] ^= 0xFF;
  EXPECT_FALSE(board.info.read_file("wifi.conf", buf, sizeof(buf)));
  EXPECT_TRUE(board.log().has_error("wifi.conf file CRC32 mismatch"));
}

TEST(BoardInfo, ReadFileNeedsAValidHeader) {
  Board board(make_image({FileSpec{"wifi.conf", std::vector<uint8_t>(64, 0x5A)}}));
  uint8_t buf[64];
  EXPECT_FALSE(board.info.read_file("wifi.conf", buf, sizeof(buf)));
  EXPECT_EQ(board.bus.reads, 0);
}

TEST(BoardInfo, DumpConfigRunsOnEveryOutcome) {
  Board v4(v4_image(signed_record(0xAB)));
  v4.boot();
  v4.info.dump_config();

  Board v3(v3_image());
  v3.boot();
  v3.info.dump_config();

  Board failed(std::vector<uint8_t>(8192, 0));
  failed.boot();
  failed.info.dump_config();
}

}  // namespace
}  // namespace esphome::jethome_board_info::testing
