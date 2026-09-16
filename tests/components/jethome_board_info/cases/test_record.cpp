// validate_record checks the device.id payload: the file's data CRC first, then the record's
// own magic, version, signature version and CRC. The two CRCs are deliberately independent.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "jeefs_vectors.h"

namespace esphome::jethome_board_info::testing {
namespace {

TEST(JeefsRecord, AcceptsTheUpstreamDevidVector) {
  const uint32_t data_crc = compute_crc32(DEVID_RECORD_V1.data(), DEVICE_ID_SIZE);
  EXPECT_EQ(validate_record(DEVID_RECORD_V1.data(), data_crc), RecordStatus::OK);
}

TEST(JeefsRecord, AcceptsABuiltRecord) {
  auto rec = make_record();
  EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec)), RecordStatus::OK);
}

TEST(JeefsRecord, RejectsAFileDataCrcThatDoesNotCoverTheRecord) {
  auto rec = make_record();
  EXPECT_EQ(validate_record(rec.data(), 0x00000000), RecordStatus::BAD_FILE_DATA_CRC);
  EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec) ^ 1u), RecordStatus::BAD_FILE_DATA_CRC);
}

TEST(JeefsRecord, RejectsABadMagic) {
  for (size_t i = 0; i < sizeof(DEVID_MAGIC); i++) {
    auto rec = make_record();
    rec[i] ^= 0x20;
    seal_record(rec);
    EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec)), RecordStatus::BAD_MAGIC) << "magic byte " << i;
  }
}

// An all-0x00 or all-0xFF buffer means "no record", which the magic check is what reports.
TEST(JeefsRecord, TreatsAnEmptyAndAnErasedRecordAsBadMagic) {
  const RecordBuf zeros{};
  RecordBuf erased{};
  erased.fill(0xFF);
  EXPECT_EQ(validate_record(zeros.data(), file_data_crc_of(zeros)), RecordStatus::BAD_MAGIC);
  EXPECT_EQ(validate_record(erased.data(), file_data_crc_of(erased)), RecordStatus::BAD_MAGIC);
}

TEST(JeefsRecord, RejectsAnyRecordVersionButOne) {
  for (uint8_t version : std::array<uint8_t, 5>{0, 2, 3, 4, 255}) {
    auto rec = make_record(version);
    EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec)), RecordStatus::UNSUPPORTED_VERSION)
        << "record_version " << static_cast<unsigned>(version);
  }
}

TEST(JeefsRecord, AcceptsEverySignatureVersionInTheEnum) {
  for (uint8_t version : std::array<uint8_t, 3>{0, 1, 2}) {
    auto rec = make_record(DEVID_RECORD_VERSION, version);
    EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec)), RecordStatus::OK)
        << "signature_version " << static_cast<unsigned>(version);
  }
}

// Spec: a signature version outside the enum is a parse error, never something to guess past.
TEST(JeefsRecord, RejectsASignatureVersionOutsideTheEnum) {
  for (uint8_t version : std::array<uint8_t, 4>{3, 4, 128, 255}) {
    auto rec = make_record(DEVID_RECORD_VERSION, version);
    EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec)), RecordStatus::UNSUPPORTED_SIGNATURE)
        << "signature_version " << static_cast<unsigned>(version);
  }
}

// The record's own CRC only becomes reachable once the file CRC is recomputed over the damage —
// which is exactly the point of carrying two of them.
TEST(JeefsRecord, RejectsACorruptedRecordCrc) {
  for (size_t i = REC_OFF_CRC32; i < DEVICE_ID_SIZE; i++) {
    auto rec = make_record();
    rec[i] ^= 0xFF;
    EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec)), RecordStatus::BAD_RECORD_CRC) << "crc byte " << i;
  }
}

TEST(JeefsRecord, EveryByteUnderTheRecordCrcIsProtected) {
  for (size_t i = 0; i < DEVICE_ID_CRC_DATA_SIZE; i++) {
    auto rec = make_record();
    rec[i] ^= 0x01;
    RecordStatus expected = RecordStatus::BAD_RECORD_CRC;
    if (i < sizeof(DEVID_MAGIC)) {
      expected = RecordStatus::BAD_MAGIC;
    } else if (i == REC_OFF_RECORD_VERSION) {
      expected = RecordStatus::UNSUPPORTED_VERSION;
    }
    EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec)), expected) << "flipped byte " << i;
  }
}

// Byte 9 is the exception the loop above cannot express: flipping bit 0 of signature_version 0
// lands on 1, which the enum allows, so only the stale record CRC catches it.
TEST(JeefsRecord, AFlippedSignatureVersionThatStaysInTheEnumIsCaughtByTheCrc) {
  auto rec = make_record();
  rec[REC_OFF_SIG_VERSION] ^= 0x01;
  EXPECT_EQ(rec[REC_OFF_SIG_VERSION], 1);
  EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec)), RecordStatus::BAD_RECORD_CRC);
}

// One CRC can fail while the other passes, in both directions.
TEST(JeefsRecord, TheTwoCrcsAreIndependent) {
  auto bad_record_crc = make_record();
  bad_record_crc[REC_OFF_CRC32] ^= 0xFF;
  EXPECT_EQ(validate_record(bad_record_crc.data(), file_data_crc_of(bad_record_crc)), RecordStatus::BAD_RECORD_CRC);

  auto bad_file_crc = make_record();
  EXPECT_EQ(validate_record(bad_file_crc.data(), file_data_crc_of(bad_file_crc)), RecordStatus::OK);
  EXPECT_EQ(validate_record(bad_file_crc.data(), 0xDEADBEEF), RecordStatus::BAD_FILE_DATA_CRC);
}

TEST(JeefsRecord, TheFileDataCrcIsCheckedBeforeAnythingInTheRecord) {
  auto rec = make_record(255, 255);
  rec[REC_OFF_MAGIC] = 'X';
  EXPECT_EQ(validate_record(rec.data(), 0x00000000), RecordStatus::BAD_FILE_DATA_CRC);
  EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec)), RecordStatus::BAD_MAGIC);
}

TEST(JeefsRecord, MagicIsCheckedBeforeVersionAndVersionBeforeSignature) {
  auto bad_magic = make_record(255, 255);
  bad_magic[REC_OFF_MAGIC] = 'X';
  seal_record(bad_magic);
  EXPECT_EQ(validate_record(bad_magic.data(), file_data_crc_of(bad_magic)), RecordStatus::BAD_MAGIC);

  auto bad_version = make_record(255, 255);
  EXPECT_EQ(validate_record(bad_version.data(), file_data_crc_of(bad_version)), RecordStatus::UNSUPPORTED_VERSION);

  auto bad_signature = make_record(DEVID_RECORD_VERSION, 3);
  bad_signature[REC_OFF_CRC32] ^= 0xFF;
  EXPECT_EQ(validate_record(bad_signature.data(), file_data_crc_of(bad_signature)),
            RecordStatus::UNSUPPORTED_SIGNATURE);
}

// Spec: readers MUST ignore reserved content — a nonzero reserved byte is not a corrupt record.
TEST(JeefsRecord, AcceptsNonZeroReservedBytesAndFlags) {
  auto rec = make_record();
  rec[REC_OFF_RESERVED1] = 0xA5;
  rec[REC_OFF_RESERVED1 + 1] = 0x5A;
  rec[REC_OFF_RESERVED2] = 0xFF;
  rec[REC_OFF_RESERVED2 + 85] = 0xFF;
  put_u16(rec.data(), REC_OFF_FLAGS, 0xBEEF);
  seal_record(rec);
  EXPECT_EQ(validate_record(rec.data(), file_data_crc_of(rec)), RecordStatus::OK);
}

// The whole identity path as a device runs it: validate the header, walk to device.id, read it.
TEST(JeefsRecord, ParsesTheWholeIdentityPath) {
  const RecordBuf record = make_record();
  const std::vector<uint8_t> image = make_image({device_id_file(record)});

  EXPECT_EQ(validate_header(image.data()), HeaderStatus::OK);
  const JeefsWalk walk = walk_image(image, DEVICE_ID_NAME);
  ASSERT_EQ(walk.status, WalkStatus::FOUND);
  EXPECT_EQ(walk.file_offset, DEVICE_ID_ADDR);
  EXPECT_EQ(walk.file_size, DEVICE_ID_SIZE);
  EXPECT_EQ(validate_record(image.data() + walk.file_offset, walk.file_crc32), RecordStatus::OK);
}

// device.id is written first today; the walker must find it when it is not.
TEST(JeefsRecord, FindsTheRecordBehindAnotherFile) {
  const RecordBuf record = make_record();
  const std::vector<uint8_t> image =
      make_image({FileSpec{"wifi.conf", std::vector<uint8_t>(64, 0x5A)}, device_id_file(record)});

  const JeefsWalk walk = walk_image(image, DEVICE_ID_NAME);
  ASSERT_EQ(walk.status, WalkStatus::FOUND);
  EXPECT_EQ(walk.file_offset, FILE_HEADER_ADDR + 2 * FILE_HEADER_SIZE + 64);
  EXPECT_EQ(validate_record(image.data() + walk.file_offset, walk.file_crc32), RecordStatus::OK);
}

}  // namespace
}  // namespace esphome::jethome_board_info::testing
