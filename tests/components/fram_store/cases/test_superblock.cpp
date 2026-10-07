// The superblock on a shared bus: one read is never the whole story, so a NACK is retried and a stale
// verdict has to repeat before the store is wiped. Also the on-chip format, pinned byte for byte.

#include "common.h"
#include "esphome/core/helpers.h"

namespace esphome::fram_store::testing {

TEST_F(FramStoreTest, OneGarbledReadDoesNotFormat) {
  uint64_t value = 42;
  ASSERT_TRUE(this->mount()->slot.save(value));
  LogCapture::instance().clear();

  this->backend_.garble_reads_covering(0, 1);
  auto mounted = this->mount();
  EXPECT_TRUE(mounted->store.is_ready());
  EXPECT_EQ(mounted->store.format_count(), 1u);
  EXPECT_TRUE(LogCapture::instance().warnings.empty());
  ASSERT_TRUE(mounted->slot.load(value));
  EXPECT_EQ(value, 42u);
}

// Two bad reads that agree are still not three: the good one after them wins.
TEST_F(FramStoreTest, TwoMatchingBadReadsDoNotFormat) {
  uint64_t value = 42;
  ASSERT_TRUE(this->mount()->slot.save(value));

  this->backend_.garble_reads_covering(0, 2, true);
  auto mounted = this->mount();
  EXPECT_EQ(mounted->store.format_count(), 1u);
  ASSERT_TRUE(mounted->slot.load(value));
  EXPECT_EQ(value, 42u);
}

// Where the line is: three answered reads returning the same stale bytes are a stale store.
TEST_F(FramStoreTest, ThreeMatchingBadReadsAreAStaleStore) {
  ASSERT_TRUE(this->mount()->store.is_ready());

  this->backend_.garble_reads_covering(0, 3, true);
  EXPECT_EQ(this->mount()->store.format_count(), 1u);
  EXPECT_TRUE(LogCapture::instance().has_warning("No superblock at 0x0000"));
}

// Bytes that never read the same twice are a bus fault, not a store to wipe.
TEST_F(FramStoreTest, ASuperblockThatNeverReadsTheSameIsLeftAlone) {
  uint64_t value = 42;
  ASSERT_TRUE(this->mount()->slot.save(value));

  this->backend_.garble_reads_covering(0, UINT32_MAX);
  auto failed = this->mount();
  EXPECT_FALSE(failed->store.is_ready());
  EXPECT_TRUE(failed->store.is_failed());
  EXPECT_TRUE(LogCapture::instance().has_error(
      "Superblock at 0x0000 reads differently each time, refusing to touch the store"));

  this->backend_.garble_reads_covering(0, 0);
  auto healthy = this->mount();
  EXPECT_EQ(healthy->store.format_count(), 1u);
  ASSERT_TRUE(healthy->slot.load(value));
  EXPECT_EQ(value, 42u);
}

// A part that misses a few reads at boot, as one on a busy bus can, is waited for, not given up on.
TEST_F(FramStoreTest, RetriesASuperblockThatDoesNotAnswer) {
  uint64_t value = 42;
  ASSERT_TRUE(this->mount()->slot.save(value));

  this->backend_.fail_reads_covering(0, 3);
  auto mounted = this->mount();
  EXPECT_TRUE(mounted->store.is_ready());
  EXPECT_EQ(mounted->store.format_count(), 1u);
  EXPECT_GE(mounted->store.init_duration_us(), 3u * 5000u) << "the retries should pause";
  ASSERT_TRUE(mounted->slot.load(value));
  EXPECT_EQ(value, 42u);
}

TEST_F(FramStoreTest, GivesUpOnASuperblockThatMissesFourReads) {
  ASSERT_TRUE(this->mount()->store.is_ready());

  this->backend_.fail_reads_covering(0, 4);
  EXPECT_FALSE(this->mount()->store.is_ready());
  EXPECT_TRUE(this->mount()->store.is_ready());
}

// A missed read between stale ones does not reset the count: a blank part that NACKs once is still formatted.
TEST_F(FramStoreTest, ABlankPartThatMissesOneReadIsStillFormatted) {
  this->backend_.fail_reads_covering(0, 1);
  auto mounted = this->mount();
  EXPECT_TRUE(mounted->store.is_ready());
  EXPECT_EQ(mounted->store.format_count(), 1u);
}

// Every superblock, slot copy and ring record carries ESPHome's crc16 with its defaults: CRC-16/MODBUS.
TEST(FramStoreFormat, ChecksAreCrc16Modbus) {
  const char *check = "123456789";
  EXPECT_EQ(crc16(reinterpret_cast<const uint8_t *>(check), 9), 0x4B37);
}

// What a blank 512-byte part holds after its first boot, and after one save to the slot.
TEST_F(FramStoreTest, WritesTheFormatByteForByte) {
  auto mounted = this->mount();
  uint64_t value = 0x0123456789ABCDEF;
  ASSERT_TRUE(mounted->slot.save(value));

  const std::vector<uint8_t> superblock = {
      'J',  'F',  'R',  'M',  0x01, 0x00, 0x20, 0x00,  // magic, version 1, 32 bytes
      0x34, 0x12, 0xCD, 0xAB,                          // layout hash
      0x00, 0x02, 0x00, 0x00,                          // a 512-byte part
      0x01, 0x00, 0x00, 0x00,                          // formatted once
      0,    0,    0,    0,    0,    0,    0,    0,    0, 0, 0x29, 0x29,
  };
  std::vector<uint8_t> read(superblock.size());
  ASSERT_TRUE(this->backend_.read(0, read.data(), read.size()));
  EXPECT_EQ(read, superblock);

  const std::vector<uint8_t> copy = {
      0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x6A, 0xBB,  // seq 1, 8 bytes, CRC
      0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
  };
  read.resize(copy.size());
  ASSERT_TRUE(this->backend_.read(SLOT_OFFSET, read.data(), read.size()));
  EXPECT_EQ(read, copy);
}

}  // namespace esphome::fram_store::testing
