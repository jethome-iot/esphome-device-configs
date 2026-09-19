// compute_crc32 is the one primitive every JEEFS structure is checked with, so it is pinned
// against published check values and against the CRCs the format owner ships in the vectors.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "jeefs_vectors.h"

namespace esphome::jethome_board_info::testing {
namespace {

uint32_t crc_of(const char *text) { return compute_crc32(reinterpret_cast<const uint8_t *>(text), strlen(text)); }

TEST(JeefsCrc32, MatchesThePublishedCheckValues) {
  EXPECT_EQ(crc_of(""), 0x00000000u);
  EXPECT_EQ(crc_of("a"), 0xE8B7BE43u);
  EXPECT_EQ(crc_of("123456789"), 0xCBF43926u);
  EXPECT_EQ(crc_of("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
}

TEST(JeefsCrc32, ZeroLengthOverANonEmptyBufferIsStillZero) {
  const std::array<uint8_t, 8> buf{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
  EXPECT_EQ(compute_crc32(buf.data(), 0), 0x00000000u);
}

TEST(JeefsCrc32, AllZeroAndAllOnesBuffersAreNotZero) {
  const std::array<uint8_t, 252> zeros{};
  std::array<uint8_t, 252> ones{};
  ones.fill(0xFF);
  EXPECT_EQ(compute_crc32(zeros.data(), zeros.size()), 0xA66359F1u);
  EXPECT_EQ(compute_crc32(ones.data(), ones.size()), 0xA7340E2Fu);
  const std::array<uint8_t, 24> zeros24{};
  EXPECT_EQ(compute_crc32(zeros24.data(), zeros24.size()), 0xA3C1CA20u);
}

TEST(JeefsCrc32, ReproducesTheCrcEveryUpstreamVectorShips) {
  EXPECT_EQ(compute_crc32(V4_HEADER_MINIMAL.data(), CRC_DATA_SIZE), V4_HEADER_MINIMAL_CRC);
  EXPECT_EQ(compute_crc32(V4_HEADER_MAXLEN.data(), CRC_DATA_SIZE), V4_HEADER_MAXLEN_CRC);
  EXPECT_EQ(compute_crc32(V3_HEADER_SIGNED.data(), CRC_DATA_SIZE), V3_HEADER_SIGNED_CRC);
  EXPECT_EQ(compute_crc32(V2_HEADER_MINIMAL.data(), CRC_DATA_SIZE), V2_HEADER_MINIMAL_CRC);
  EXPECT_EQ(compute_crc32(DEVID_RECORD_V1.data(), DEVICE_ID_CRC_DATA_SIZE), DEVID_RECORD_V1_CRC);
}

TEST(JeefsCrc32, TheShippedCrcIsWhatTheVectorStoresAtOffset252) {
  EXPECT_EQ(get_u32(V4_HEADER_MINIMAL.data(), HDR_OFF_CRC32), V4_HEADER_MINIMAL_CRC);
  EXPECT_EQ(get_u32(V3_HEADER_SIGNED.data(), HDR_OFF_CRC32), V3_HEADER_SIGNED_CRC);
  EXPECT_EQ(get_u32(DEVID_RECORD_V1.data(), REC_OFF_CRC32), DEVID_RECORD_V1_CRC);
}

// CRC-32 over data followed by its own little-endian CRC is the constant residue, which is why
// every sealed 256-byte structure has the same file-data CRC — do not read that as a copy-paste.
TEST(JeefsCrc32, ASealedStructureCarriesTheStandardResidue) {
  EXPECT_EQ(compute_crc32(V4_HEADER_MINIMAL.data(), BOARD_DATA_SIZE), 0x2144DF1Cu);
  EXPECT_EQ(compute_crc32(DEVID_RECORD_V1.data(), DEVICE_ID_SIZE), 0x2144DF1Cu);
}

TEST(JeefsCrc32, OneFlippedBitChangesTheResult) {
  auto buf = V4_HEADER_MINIMAL;
  buf[128] ^= 0x01;
  EXPECT_NE(compute_crc32(buf.data(), CRC_DATA_SIZE), V4_HEADER_MINIMAL_CRC);
}

}  // namespace
}  // namespace esphome::jethome_board_info::testing
