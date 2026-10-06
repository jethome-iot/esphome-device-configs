// What a header check and a chain walk come to for a caller: the one status read_file returns.

#include <gtest/gtest.h>

#include "common.h"

namespace esphome::jethome_board_info::testing {
namespace {

TEST(ReadStatus, ANoMagicHeaderIsNoHeaderAndABadCrcIsCorrupt) {
  EXPECT_EQ(header_read_status(HeaderStatus::OK), ReadStatus::OK);
  EXPECT_EQ(header_read_status(HeaderStatus::BAD_MAGIC), ReadStatus::NO_HEADER);
  EXPECT_EQ(header_read_status(HeaderStatus::UNSUPPORTED_VERSION), ReadStatus::UNSUPPORTED);
  EXPECT_EQ(header_read_status(HeaderStatus::BAD_CRC), ReadStatus::CORRUPT);
  EXPECT_EQ(header_read_status(static_cast<HeaderStatus>(0xEE)), ReadStatus::CORRUPT);
}

TEST(ReadStatus, AWalkEndsInTheStatusItsCallerActsOn) {
  EXPECT_EQ(walk_read_status(WalkStatus::FOUND, FS_VERSION_JEEFS_V1), ReadStatus::OK);
  EXPECT_EQ(walk_read_status(WalkStatus::NOT_FOUND, FS_VERSION_JEEFS_V1), ReadStatus::NOT_FOUND);
  EXPECT_EQ(walk_read_status(WalkStatus::BAD_NAME, FS_VERSION_JEEFS_V1), ReadStatus::BAD_NAME);
  EXPECT_EQ(walk_read_status(WalkStatus::BAD_FILE_HEADER_CRC, FS_VERSION_JEEFS_V1), ReadStatus::CORRUPT);
  EXPECT_EQ(walk_read_status(WalkStatus::CORRUPT_CHAIN, FS_VERSION_JEEFS_V1), ReadStatus::CORRUPT);
  EXPECT_EQ(walk_read_status(WalkStatus::WANT_HEADER, FS_VERSION_JEEFS_V1), ReadStatus::READ_ERROR);
}

// No file area is no file; a file area this reader cannot walk is a format it does not know.
TEST(ReadStatus, NoFilesystemIsNotFoundOnlyWhenTheHeaderSaysSo) {
  EXPECT_EQ(walk_read_status(WalkStatus::NO_FILESYSTEM, FS_VERSION_NONE), ReadStatus::NOT_FOUND);
  EXPECT_EQ(walk_read_status(WalkStatus::NO_FILESYSTEM, 7), ReadStatus::UNSUPPORTED);
}

TEST(ReadStatus, EveryStatusHasAName) {
  EXPECT_STREQ(read_status_str(ReadStatus::OK), "OK");
  EXPECT_STREQ(read_status_str(ReadStatus::NO_EEPROM), "NO_EEPROM");
  EXPECT_STREQ(read_status_str(ReadStatus::NO_HEADER), "NO_HEADER");
  EXPECT_STREQ(read_status_str(ReadStatus::UNSUPPORTED), "UNSUPPORTED");
  EXPECT_STREQ(read_status_str(ReadStatus::CORRUPT), "CORRUPT");
  EXPECT_STREQ(read_status_str(ReadStatus::NOT_FOUND), "NOT_FOUND");
  EXPECT_STREQ(read_status_str(ReadStatus::TOO_LARGE), "TOO_LARGE");
  EXPECT_STREQ(read_status_str(ReadStatus::BAD_NAME), "BAD_NAME");
  EXPECT_STREQ(read_status_str(ReadStatus::READ_ERROR), "READ_ERROR");
  EXPECT_STREQ(read_status_str(static_cast<ReadStatus>(0xEE)), "UNKNOWN");
  EXPECT_EQ(::testing::PrintToString(ReadStatus::CORRUPT), "CORRUPT");
}

}  // namespace
}  // namespace esphome::jethome_board_info::testing
