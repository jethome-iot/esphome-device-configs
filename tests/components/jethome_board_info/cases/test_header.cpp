// validate_header gates every field the component publishes, and its three checks are ordered
// magic -> version -> CRC; each test below pins one rung of that ladder.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "jeefs_vectors.h"

namespace esphome::jethome_board_info::testing {
namespace {

TEST(JeefsHeader, AcceptsTheUpstreamV4Vectors) {
  EXPECT_EQ(validate_header(V4_HEADER_MINIMAL.data()), HeaderStatus::OK);
  EXPECT_EQ(validate_header(V4_HEADER_MAXLEN.data()), HeaderStatus::OK);
}

TEST(JeefsHeader, AcceptsTheUpstreamV3Vector) {
  EXPECT_EQ(validate_header(V3_HEADER_SECP256R1.data()), HeaderStatus::OK);
}

// v1 is 512 bytes and v2 has no signature, so parsing either as v3/v4 would invent fields.
TEST(JeefsHeader, RejectsTheUpstreamV1AndV2Vectors) {
  EXPECT_EQ(validate_header(V1_HEADER_MINIMAL_PREFIX.data()), HeaderStatus::UNSUPPORTED_VERSION);
  EXPECT_EQ(validate_header(V2_HEADER_MINIMAL.data()), HeaderStatus::UNSUPPORTED_VERSION);
}

TEST(JeefsHeader, AcceptsABuiltV3AndV4) {
  EXPECT_EQ(validate_header(make_header(HEADER_VERSION_V3).data()), HeaderStatus::OK);
  EXPECT_EQ(validate_header(make_header(HEADER_VERSION_V4).data()), HeaderStatus::OK);
}

TEST(JeefsHeader, RejectsEveryOtherVersionEvenWithAGoodCrc) {
  for (uint8_t version : std::array<uint8_t, 6>{0, 1, 2, 5, 6, 255}) {
    auto buf = make_header(version);
    EXPECT_EQ(validate_header(buf.data()), HeaderStatus::UNSUPPORTED_VERSION)
        << "version " << static_cast<unsigned>(version);
  }
}

TEST(JeefsHeader, RejectsABadMagicByte) {
  for (size_t i = 0; i < sizeof(BOARD_MAGIC); i++) {
    auto buf = make_header();
    buf[i] ^= 0x20;
    seal_header(buf);
    EXPECT_EQ(validate_header(buf.data()), HeaderStatus::BAD_MAGIC) << "magic byte " << i;
  }
}

// The magic is compared over all 8 bytes, so a name that merely starts with JETHOME is not one.
TEST(JeefsHeader, RejectsAMagicWithoutItsTerminator) {
  auto buf = make_header();
  buf[7] = 'X';
  seal_header(buf);
  EXPECT_EQ(validate_header(buf.data()), HeaderStatus::BAD_MAGIC);
}

TEST(JeefsHeader, TreatsAnEmptyOrErasedSlotAsBadMagic) {
  const HeaderBuf zeros{};
  HeaderBuf erased{};
  erased.fill(0xFF);
  EXPECT_EQ(validate_header(zeros.data()), HeaderStatus::BAD_MAGIC);
  EXPECT_EQ(validate_header(erased.data()), HeaderStatus::BAD_MAGIC);
}

// Every byte the CRC covers must be covered: flipping one bit anywhere in 0..251 has to be
// caught, by the magic check, the version check or the CRC, depending on where it lands.
TEST(JeefsHeader, EveryByteUnderTheCrcIsProtected) {
  for (size_t i = 0; i < CRC_DATA_SIZE; i++) {
    auto buf = make_header();
    buf[i] ^= 0x01;
    HeaderStatus expected = HeaderStatus::BAD_CRC;
    if (i < sizeof(BOARD_MAGIC)) {
      expected = HeaderStatus::BAD_MAGIC;
    } else if (i == HDR_OFF_VERSION) {
      expected = HeaderStatus::UNSUPPORTED_VERSION;
    }
    EXPECT_EQ(validate_header(buf.data()), expected) << "flipped byte " << i;
  }
}

// The stored CRC itself is outside the covered range, so corrupting it is a plain CRC failure.
TEST(JeefsHeader, RejectsACorruptedStoredCrc) {
  for (size_t i = HDR_OFF_CRC32; i < BOARD_DATA_SIZE; i++) {
    auto buf = make_header();
    buf[i] ^= 0xFF;
    EXPECT_EQ(validate_header(buf.data()), HeaderStatus::BAD_CRC) << "crc byte " << i;
  }
}

TEST(JeefsHeader, MagicIsCheckedBeforeVersionAndVersionBeforeCrc) {
  auto bad_magic_and_version = make_header(5);
  bad_magic_and_version[0] = 'X';
  seal_header(bad_magic_and_version);
  EXPECT_EQ(validate_header(bad_magic_and_version.data()), HeaderStatus::BAD_MAGIC);

  auto bad_magic_and_crc = make_header();
  bad_magic_and_crc[0] = 'X';
  EXPECT_EQ(validate_header(bad_magic_and_crc.data()), HeaderStatus::BAD_MAGIC);

  auto bad_version_and_crc = make_header();
  bad_version_and_crc[HDR_OFF_VERSION] = 2;
  EXPECT_EQ(validate_header(bad_version_and_crc.data()), HeaderStatus::UNSUPPORTED_VERSION);
}

// A v4 header is byte-identical to v3, so only the version byte separates the two.
TEST(JeefsHeader, TheUpstreamV4VectorStaysValidAsV3AndNothingElse) {
  auto buf = V4_HEADER_MINIMAL;
  buf[HDR_OFF_VERSION] = HEADER_VERSION_V3;
  seal_header(buf);
  EXPECT_EQ(validate_header(buf.data()), HeaderStatus::OK);

  buf[HDR_OFF_VERSION] = 5;
  seal_header(buf);
  EXPECT_EQ(validate_header(buf.data()), HeaderStatus::UNSUPPORTED_VERSION);
}

// fs_version and signature_version are read, never gated on — an unknown one is not a header fault.
TEST(JeefsHeader, DoesNotGateOnFsVersionOrSignatureVersion) {
  for (uint8_t value : std::array<uint8_t, 5>{0, 1, 2, 3, 255}) {
    auto buf = make_header();
    buf[HDR_OFF_FS_VERSION] = value;
    buf[HDR_OFF_SIG_VERSION] = value;
    seal_header(buf);
    EXPECT_EQ(validate_header(buf.data()), HeaderStatus::OK) << "value " << static_cast<unsigned>(value);
  }
}

}  // namespace
}  // namespace esphome::jethome_board_info::testing
