// The chain walker: the only thing that decides where a file is, now that device.id is no
// longer read from a fixed offset. Rules and their order mirror upstream's jeefs_walk.c.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "jeefs_vectors.h"

namespace esphome::jethome_board_info::testing {
namespace {

const std::vector<uint8_t> SOME_DATA(64, 0x5A);

void put_slot(std::vector<uint8_t> &image, size_t at, const FileHeaderBuf &slot) {
  memcpy(image.data() + at, slot.data(), slot.size());
}

// Rewrites a slot's header CRC after a test has changed a field under it.
void reseal_slot(std::vector<uint8_t> &image, size_t at) {
  put_u32(image.data() + at, FH_OFF_HEADER_CRC32, compute_crc32(image.data() + at, FILE_HEADER_CRC_DATA_SIZE));
}

TEST(JeefsWalk, FindsTheOnlyFile) {
  const std::vector<uint8_t> image = make_image({FileSpec{"wifi.conf", SOME_DATA}});
  const JeefsWalk walk = walk_image(image, "wifi.conf");
  EXPECT_EQ(walk.status, WalkStatus::FOUND);
  EXPECT_EQ(walk.file_offset, FILE_HEADER_ADDR + FILE_HEADER_SIZE);
  EXPECT_EQ(walk.file_size, SOME_DATA.size());
  EXPECT_EQ(walk.file_crc32, compute_crc32(SOME_DATA.data(), SOME_DATA.size()));
}

TEST(JeefsWalk, FindsEveryFileOfAThreeFileChain) {
  const std::vector<uint8_t> image =
      make_image({FileSpec{"a.bin", std::vector<uint8_t>(16, 1)}, FileSpec{"b.bin", std::vector<uint8_t>(32, 2)},
                  FileSpec{"c.bin", std::vector<uint8_t>(48, 3)}});
  EXPECT_EQ(walk_image(image, "a.bin").file_offset, FILE_HEADER_ADDR + FILE_HEADER_SIZE);
  EXPECT_EQ(walk_image(image, "b.bin").file_offset, FILE_HEADER_ADDR + 2 * FILE_HEADER_SIZE + 16);
  EXPECT_EQ(walk_image(image, "c.bin").file_offset, FILE_HEADER_ADDR + 3 * FILE_HEADER_SIZE + 48);
  for (const char *name : {"a.bin", "b.bin", "c.bin"}) {
    EXPECT_EQ(walk_image(image, name).status, WalkStatus::FOUND) << name;
  }
}

TEST(JeefsWalk, ReportsNotFoundForANameThatIsNotThere) {
  const std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}, FileSpec{"b.bin", SOME_DATA}});
  EXPECT_EQ(walk_image(image, "c.bin").status, WalkStatus::NOT_FOUND);
}

// The name is compared whole: neither a prefix nor an extension of it is the file.
TEST(JeefsWalk, MatchesTheNameExactly) {
  const std::vector<uint8_t> image = make_image({device_id_file(make_record())});
  for (const char *name : {"device.i", "device.idx", "Device.id", "evice.id"}) {
    EXPECT_EQ(walk_image(image, name).status, WalkStatus::NOT_FOUND) << name;
  }
  EXPECT_EQ(walk_image(image, DEVICE_ID_NAME).status, WalkStatus::FOUND);
}

// Spec: an unwritten slot is the end of the chain, in both empty domains, and never corruption.
TEST(JeefsWalk, TreatsAnEmptyAndAnErasedSlotAsTheEndOfTheChain) {
  std::vector<uint8_t> zeros = make_image({});
  EXPECT_EQ(walk_image(zeros, "a.bin").status, WalkStatus::NOT_FOUND);

  std::vector<uint8_t> erased = make_image({});
  memset(erased.data() + FILE_HEADER_ADDR, 0xFF, FILE_HEADER_SIZE);
  EXPECT_EQ(walk_image(erased, "a.bin").status, WalkStatus::NOT_FOUND);
}

TEST(JeefsWalk, ReportsNoFilesystemWhenTheHeaderDeclaresNone) {
  const std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}}, 8192, FS_VERSION_NONE);
  const JeefsWalk walk = walk_image(image, "a.bin");
  EXPECT_EQ(walk.status, WalkStatus::NO_FILESYSTEM);
}

// Asserted on the request, not on the status: an empty first slot ends the chain with the same
// NOT_FOUND, so only "asked for nothing" distinguishes the bound check from that.
TEST(JeefsWalk, AsksForNothingWhenTheImageHasNoRoomForAFileHeader) {
  const std::vector<uint8_t> image = make_image({}, FILE_HEADER_ADDR + FILE_HEADER_SIZE - 1);
  JeefsWalk walk{};
  ASSERT_EQ(walk_begin(&walk, FS_VERSION_JEEFS_V1, image.size(), "a.bin"), WalkStatus::NOT_FOUND);
  uint16_t at = 0;
  EXPECT_FALSE(walk_want(&walk, &at));
}

// A target truncated to fit would compare equal to a shorter file and return the wrong one.
TEST(JeefsWalk, RejectsANameThatDoesNotFitWithItsTerminator) {
  const std::vector<uint8_t> image = make_image({FileSpec{"0123456789ABCDE", SOME_DATA}});
  EXPECT_EQ(walk_image(image, "0123456789ABCDE").status, WalkStatus::FOUND);
  EXPECT_EQ(walk_image(image, "0123456789ABCDEF").status, WalkStatus::BAD_NAME);
  EXPECT_EQ(walk_image(image, "0123456789ABCDEFGHIJ").status, WalkStatus::BAD_NAME);
}

TEST(JeefsWalk, RejectsAnEmptyAndANullName) {
  const std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}});
  EXPECT_EQ(walk_image(image, "").status, WalkStatus::BAD_NAME);

  JeefsWalk walk{};
  EXPECT_EQ(walk_begin(&walk, FS_VERSION_JEEFS_V1, image.size(), nullptr), WalkStatus::BAD_NAME);
}

// The first slot is the easy one; a link that goes bad only on the second pass must too.
TEST(JeefsWalk, CatchesACorruptedSlotLaterInTheChain) {
  const size_t second = FILE_HEADER_ADDR + FILE_HEADER_SIZE + SOME_DATA.size();
  std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}, FileSpec{"b.bin", SOME_DATA}});
  image[second + FH_OFF_DATA_SIZE] ^= 0xFF;
  EXPECT_EQ(walk_image(image, "b.bin").status, WalkStatus::BAD_FILE_HEADER_CRC);

  put_u16(image.data() + second, FH_OFF_DATA_SIZE, 0);
  reseal_slot(image, second);
  EXPECT_EQ(walk_image(image, "b.bin").status, WalkStatus::CORRUPT_CHAIN);
  EXPECT_EQ(walk_image(image, "a.bin").status, WalkStatus::FOUND);
}

TEST(JeefsWalk, EveryByteUnderTheSlotHeaderCrcIsProtected) {
  for (size_t i = 0; i < FILE_HEADER_CRC_DATA_SIZE; i++) {
    std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}});
    image[FILE_HEADER_ADDR + i] ^= 0x01;
    EXPECT_EQ(walk_image(image, "a.bin").status, WalkStatus::BAD_FILE_HEADER_CRC) << "flipped byte " << i;
  }
}

TEST(JeefsWalk, RejectsACorruptedSlotHeaderCrc) {
  for (size_t i = FH_OFF_HEADER_CRC32; i < FILE_HEADER_SIZE; i++) {
    std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}});
    image[FILE_HEADER_ADDR + i] ^= 0xFF;
    EXPECT_EQ(walk_image(image, "a.bin").status, WalkStatus::BAD_FILE_HEADER_CRC) << "crc byte " << i;
  }
}

// A name filling all 16 bytes leaves no terminator, so no reader could tell where it ends.
TEST(JeefsWalk, RejectsAnUnterminatedName) {
  std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}});
  image[FILE_HEADER_ADDR + FILE_NAME_SIZE - 1] = 'X';
  reseal_slot(image, FILE_HEADER_ADDR);
  EXPECT_EQ(walk_image(image, "a.bin").status, WalkStatus::CORRUPT_CHAIN);
}

TEST(JeefsWalk, RejectsAnEmptyAndAnErasedDataSize) {
  for (uint16_t size : {uint16_t{0}, uint16_t{0xFFFF}}) {
    std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}});
    put_u16(image.data() + FILE_HEADER_ADDR, FH_OFF_DATA_SIZE, size);
    reseal_slot(image, FILE_HEADER_ADDR);
    EXPECT_EQ(walk_image(image, "a.bin").status, WalkStatus::CORRUPT_CHAIN) << "size " << size;
  }
}

TEST(JeefsWalk, RejectsAFileThatRunsPastTheImage) {
  std::vector<uint8_t> image = make_image({}, 400);
  put_slot(image, FILE_HEADER_ADDR, make_file_header("a.bin", 200, 0));
  EXPECT_EQ(walk_image(image, "a.bin").status, WalkStatus::CORRUPT_CHAIN);
}

// An erased link reads 0xFFFF and must end the chain exactly as 0 does.
TEST(JeefsWalk, TreatsAnErasedLinkAsTheTerminator) {
  std::vector<uint8_t> image = make_image({});
  put_slot(image, FILE_HEADER_ADDR, make_file_header("a.bin", 64, 0, NEXT_TERMINAL));
  EXPECT_EQ(walk_image(image, "a.bin").status, WalkStatus::FOUND);
  EXPECT_EQ(walk_image(image, "b.bin").status, WalkStatus::NOT_FOUND);
}

// Files are contiguous, so a link that does not land exactly on the previous file's end is a
// chain nobody wrote — following it would read a header out of somebody's data.
TEST(JeefsWalk, RejectsALinkThatDoesNotAbutTheFile) {
  const uint16_t end = FILE_HEADER_ADDR + FILE_HEADER_SIZE + 64;
  for (uint16_t next : {static_cast<uint16_t>(end - 1), static_cast<uint16_t>(end + 1)}) {
    std::vector<uint8_t> image = make_image({});
    put_slot(image, FILE_HEADER_ADDR, make_file_header("a.bin", 64, 0, next));
    EXPECT_EQ(walk_image(image, "b.bin").status, WalkStatus::CORRUPT_CHAIN) << "next " << next;
  }
}

TEST(JeefsWalk, RejectsALinkWithNoRoomForItsHeader) {
  const uint16_t image_size = FILE_HEADER_ADDR + FILE_HEADER_SIZE + 64 + FILE_HEADER_SIZE - 1;
  std::vector<uint8_t> image = make_image({}, image_size);
  put_slot(image, FILE_HEADER_ADDR, make_file_header("a.bin", 64, 0, FILE_HEADER_ADDR + FILE_HEADER_SIZE + 64));
  EXPECT_EQ(walk_image(image, "b.bin").status, WalkStatus::CORRUPT_CHAIN);
}

// The CRC gates every other field, so a slot that is both corrupt and malformed reports the CRC.
TEST(JeefsWalk, ChecksTheHeaderCrcBeforeAnyFieldUnderIt) {
  std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}});
  put_u16(image.data() + FILE_HEADER_ADDR, FH_OFF_DATA_SIZE, 0);
  image[FILE_HEADER_ADDR + FILE_NAME_SIZE - 1] = 'X';
  EXPECT_EQ(walk_image(image, "a.bin").status, WalkStatus::BAD_FILE_HEADER_CRC);
}

// A found file's data CRC is carried for the caller to check, never checked by the walk itself.
TEST(JeefsWalk, CarriesTheDataCrcWithoutValidatingIt) {
  std::vector<uint8_t> image = make_image({});
  put_slot(image, FILE_HEADER_ADDR, make_file_header("a.bin", 64, 0xDEADBEEF));
  const JeefsWalk walk = walk_image(image, "a.bin");
  EXPECT_EQ(walk.status, WalkStatus::FOUND);
  EXPECT_EQ(walk.file_crc32, 0xDEADBEEFu);
}

TEST(JeefsWalk, StopsAskingForBytesOnceTerminal) {
  const std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}});
  JeefsWalk walk = walk_image(image, "a.bin");
  ASSERT_EQ(walk.status, WalkStatus::FOUND);
  uint16_t at = 0;
  EXPECT_FALSE(walk_want(&walk, &at));
}

// Feeding a terminal walker must not restart it or change the answer it already gave.
TEST(JeefsWalk, IsIdempotentAfterATerminalState) {
  const std::vector<uint8_t> image = make_image({FileSpec{"a.bin", SOME_DATA}});
  JeefsWalk walk = walk_image(image, "a.bin");
  const uint32_t offset = walk.file_offset;
  EXPECT_EQ(walk_feed(&walk, image.data() + FILE_HEADER_ADDR), WalkStatus::FOUND);
  EXPECT_EQ(walk.file_offset, offset);
}

}  // namespace
}  // namespace esphome::jethome_board_info::testing
