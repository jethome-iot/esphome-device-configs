// The superblock, the slots and the store's own guards, over a RamBackend that can be told to fail.

#include "common.h"
#include "esphome/core/helpers.h"

namespace esphome::fram_store::testing {

TEST_F(FramStoreTest, FormatsABlankDevice) {
  auto mounted = this->mount();

  EXPECT_TRUE(mounted->store.is_ready());
  EXPECT_EQ(mounted->store.format_count(), 1u);
  EXPECT_FALSE(mounted->slot.has_data());
  EXPECT_EQ(mounted->ring.count(), 0u);
}

TEST_F(FramStoreTest, KeepsTheSuperblockAcrossRestarts) {
  EXPECT_EQ(this->mount()->store.format_count(), 1u);
  EXPECT_EQ(this->mount()->store.format_count(), 1u);
}

TEST_F(FramStoreTest, ReformatsWhenTheLayoutChanged) {
  EXPECT_EQ(this->mount()->store.format_count(), 1u);
  EXPECT_EQ(this->mount(0x99999999)->store.format_count(), 2u);
}

// A transient NACK on the shared bus must not read as "blank device" and wipe live data.
TEST_F(FramStoreTest, DoesNotFormatWhenTheSuperblockCannotBeRead) {
  uint64_t written = 99;
  ASSERT_TRUE(this->mount()->slot.save(written));

  this->backend_.fail_reads_covering(0);
  auto failed = this->mount();
  EXPECT_FALSE(failed->store.is_ready());

  this->backend_.stop_failing();
  auto healthy = this->mount();
  EXPECT_EQ(healthy->store.format_count(), 1u);
  uint64_t read = 0;
  ASSERT_TRUE(healthy->slot.load(read));
  EXPECT_EQ(read, 99u);
}

TEST_F(FramStoreTest, SlotRoundTrips) {
  auto mounted = this->mount();
  uint64_t written = 0x0123456789ABCDEF;

  ASSERT_TRUE(mounted->slot.save(written));
  EXPECT_TRUE(mounted->slot.has_data());

  uint64_t read = 0;
  ASSERT_TRUE(mounted->slot.load(read));
  EXPECT_EQ(read, written);
}

TEST_F(FramStoreTest, SlotSurvivesARestart) {
  uint64_t written = 42;
  ASSERT_TRUE(this->mount()->slot.save(written));

  auto mounted = this->mount();
  uint64_t read = 0;
  ASSERT_TRUE(mounted->slot.load(read));
  EXPECT_EQ(read, 42u);
  EXPECT_EQ(mounted->store.format_count(), 1u);
}

TEST_F(FramStoreTest, SlotAlternatesCopies) {
  auto mounted = this->mount();
  uint64_t value = 1;

  ASSERT_TRUE(mounted->slot.save(value));
  uint32_t first_seq = mounted->slot.seq();
  value = 2;
  ASSERT_TRUE(mounted->slot.save(value));

  EXPECT_GT(mounted->slot.seq(), first_seq);
}

TEST_F(FramStoreTest, SlotFallsBackToThePreviousCopy) {
  {
    auto mounted = this->mount();
    uint64_t value = 111;
    ASSERT_TRUE(mounted->slot.save(value));
    value = 222;
    ASSERT_TRUE(mounted->slot.save(value));
  }

  // The second save landed in copy 1; damaging its payload leaves copy 0 as the newest valid one.
  this->corrupt(slot_payload(1));

  auto mounted = this->mount();
  uint64_t read = 0;
  ASSERT_TRUE(mounted->slot.load(read));
  EXPECT_EQ(read, 111u);
}

// init() proves the copy was good at boot; load() has to prove it is still good.
TEST_F(FramStoreTest, SlotLoadRejectsACopyDamagedAfterInit) {
  auto mounted = this->mount();
  uint64_t value = 7;
  ASSERT_TRUE(mounted->slot.save(value));

  this->corrupt(slot_payload(0));

  uint64_t read = 0;
  EXPECT_FALSE(mounted->slot.load(read));
}

// Numbering from scratch after a damaged copy would let the older copy win on the next boot.
TEST_F(FramStoreTest, SlotNumbersAboveWhatIsOnTheDevice) {
  {
    auto mounted = this->mount();
    uint64_t value = 111;
    ASSERT_TRUE(mounted->slot.save(value));
    value = 222;
    ASSERT_TRUE(mounted->slot.save(value));
  }
  this->corrupt(slot_payload(1));

  {
    auto mounted = this->mount();
    EXPECT_EQ(mounted->slot.seq(), 1u);
    uint64_t value = 333;
    ASSERT_TRUE(mounted->slot.save(value));
    EXPECT_GT(mounted->slot.seq(), 2u);
  }

  auto mounted = this->mount();
  uint64_t read = 0;
  ASSERT_TRUE(mounted->slot.load(read));
  EXPECT_EQ(read, 333u);
}

// A copy that could not be read is a copy whose sequence number is unknown: writing the other one could
// resurrect it, so the slot refuses instead.
TEST_F(FramStoreTest, SlotRefusesToSaveWhenACopyCannotBeRead) {
  uint64_t written = 5;
  ASSERT_TRUE(this->mount()->slot.save(written));

  this->backend_.fail_reads_covering(slot_copy(1));
  auto mounted = this->mount();

  EXPECT_TRUE(mounted->store.is_ready());
  EXPECT_FALSE(mounted->slot.is_initialised());
  uint64_t value = 6;
  EXPECT_FALSE(mounted->slot.save(value));
  uint64_t read = 0;
  EXPECT_FALSE(mounted->slot.load(read));
}

// The payload read is where a CRC check meets the bus: a failure there says nothing about the data.
TEST_F(FramStoreTest, SlotRefusesToSaveWhenAPayloadCannotBeRead) {
  uint64_t written = 5;
  ASSERT_TRUE(this->mount()->slot.save(written));

  this->backend_.fail_reads_covering(slot_payload(0));
  auto mounted = this->mount();
  EXPECT_TRUE(mounted->store.is_ready());
  EXPECT_FALSE(mounted->slot.is_initialised());
  uint64_t value = 6;
  EXPECT_FALSE(mounted->slot.save(value));

  this->backend_.stop_failing();
  auto healthy = this->mount();
  uint64_t read = 0;
  ASSERT_TRUE(healthy->slot.load(read));
  EXPECT_EQ(read, 5u);
}

TEST_F(FramStoreTest, SlotRejectsTheWrongSize) {
  auto mounted = this->mount();
  uint64_t stored = 1;
  ASSERT_TRUE(mounted->slot.save(stored));

  uint32_t too_small = 7;
  EXPECT_FALSE(mounted->slot.save(too_small));
  EXPECT_FALSE(mounted->slot.load(too_small));
  uint64_t unchanged = 0;
  ASSERT_TRUE(mounted->slot.load(unchanged));
  EXPECT_EQ(unchanged, 1u);
}

TEST_F(FramStoreTest, ClearEmptiesARegion) {
  auto mounted = this->mount();
  uint64_t value = 7;

  ASSERT_TRUE(mounted->slot.save(value));
  ASSERT_TRUE(mounted->ring.append(value));

  ASSERT_TRUE(mounted->slot.clear());
  ASSERT_TRUE(mounted->ring.clear());

  EXPECT_FALSE(mounted->slot.has_data());
  EXPECT_EQ(mounted->ring.count(), 0u);
  EXPECT_FALSE(mounted->slot.load(value));
}

TEST_F(FramStoreTest, FormatWipesEverything) {
  auto mounted = this->mount();
  uint64_t value = 7;

  ASSERT_TRUE(mounted->slot.save(value));
  ASSERT_TRUE(mounted->ring.append(value));

  ASSERT_TRUE(mounted->store.format());

  EXPECT_EQ(mounted->store.format_count(), 2u);
  EXPECT_FALSE(mounted->slot.has_data());
  EXPECT_EQ(mounted->ring.count(), 0u);
}

TEST_F(FramStoreTest, RefusesADeviceOfTheWrongSize) {
  RamBackend smaller{DEVICE_SIZE / 2};
  FramStore store;
  store.set_backend(&smaller);
  store.set_expected_size(DEVICE_SIZE);
  store.set_layout_hash(LAYOUT_HASH);
  store.setup();

  EXPECT_FALSE(store.is_ready());
  uint8_t byte = 0;
  EXPECT_FALSE(store.read(0, &byte, 1));
}

TEST_F(FramStoreTest, RefusesAccessPastTheDevice) {
  auto mounted = this->mount();
  uint8_t buffer[8] = {};

  EXPECT_FALSE(mounted->store.read(DEVICE_SIZE - 4, buffer, sizeof(buffer)));
  EXPECT_FALSE(mounted->store.write(DEVICE_SIZE - 4, buffer, sizeof(buffer)));
}

// An uninitialised region does not know what it holds, so erasing it is as wrong as writing to it.
TEST_F(FramStoreTest, ClearRefusesOnAnUnreadableRegion) {
  uint64_t written = 3;
  ASSERT_TRUE(this->mount()->slot.save(written));

  this->backend_.fail_reads_covering(slot_copy(0));
  auto mounted = this->mount();
  EXPECT_FALSE(mounted->slot.is_initialised());
  EXPECT_FALSE(mounted->slot.clear());

  this->backend_.stop_failing();
  auto healthy = this->mount();
  uint64_t read = 0;
  ASSERT_TRUE(healthy->slot.load(read));
  EXPECT_EQ(read, 3u);
}

// A consumer that sets up before the store still gets a live one by asking for it.
TEST_F(FramStoreTest, IsReadyOpensTheStoreBeforeSetup) {
  FramStore store;
  FramSlot slot(&store, SLOT_OFFSET, SLOT_PAYLOAD);
  store.set_backend(&this->backend_);
  store.set_expected_size(DEVICE_SIZE);
  store.set_layout_hash(LAYOUT_HASH);
  store.add_region(&slot);

  EXPECT_TRUE(store.is_ready());
  EXPECT_TRUE(slot.is_initialised());
  uint64_t value = 3;
  EXPECT_TRUE(slot.save(value));
  store.setup();
  EXPECT_EQ(store.format_count(), 1u);
}

TEST_F(FramStoreTest, RefusesToRunWithoutABackend) {
  FramStore store;
  store.setup();

  EXPECT_FALSE(store.is_ready());
  EXPECT_TRUE(store.is_failed());
  EXPECT_STREQ(store.backend_type(), "none");
  EXPECT_FALSE(store.format());
  EXPECT_TRUE(LogCapture::instance().has_error("No usable memory device"));
}

// A format that cannot write leaves neither the old contents nor a store that claims to work.
TEST_F(FramStoreTest, AFailedFormatLeavesTheStoreUnusable) {
  this->backend_.fail_writes(true);
  auto mounted = this->mount();

  EXPECT_FALSE(mounted->store.is_ready());
  EXPECT_TRUE(mounted->store.is_failed());
  EXPECT_TRUE(LogCapture::instance().has_error("Formatting failed, store is unusable"));
  uint64_t value = 1;
  EXPECT_FALSE(mounted->slot.save(value));
}

// A superblock that fails its CRC is stale, not unreadable: the device answered, so it is formatted.
TEST_F(FramStoreTest, ReformatsWhenTheSuperblockIsDamaged) {
  uint64_t written = 8;
  ASSERT_TRUE(this->mount()->slot.save(written));

  uint8_t byte = 0xEE;
  ASSERT_TRUE(this->backend_.write(20, &byte, 1));  // inside the reserved bytes

  auto mounted = this->mount();
  EXPECT_TRUE(mounted->store.is_ready());
  EXPECT_FALSE(mounted->slot.has_data());
  // The count could not be trusted either, so it starts again.
  EXPECT_EQ(mounted->store.format_count(), 1u);
}

TEST_F(FramStoreTest, ReformatsWhenTheMagicIsWrong) {
  ASSERT_TRUE(this->mount()->store.is_ready());
  uint8_t magic[4] = {'X', 'X', 'X', 'X'};
  ASSERT_TRUE(this->backend_.write(0, magic, sizeof(magic)));
  EXPECT_EQ(this->mount()->store.format_count(), 1u);
}

// The superblock names the part's size: one written for another size is someone else's layout.
TEST_F(FramStoreTest, ReformatsWhenTheSuperblockNamesAnotherSize) {
  ASSERT_TRUE(this->mount()->store.is_ready());

  uint8_t block[SUPERBLOCK_SIZE];
  ASSERT_TRUE(this->backend_.read(0, block, sizeof(block)));
  const uint32_t other = DEVICE_SIZE * 2;
  std::memcpy(block + 12, &other, sizeof(other));
  const uint16_t crc = crc16(block, sizeof(block) - 2);
  std::memcpy(block + sizeof(block) - 2, &crc, sizeof(crc));
  ASSERT_TRUE(this->backend_.write(0, block, sizeof(block)));

  EXPECT_EQ(this->mount()->store.format_count(), 2u);
}

TEST_F(FramStoreTest, FillRefusesASpanPastTheEndWhole) {
  auto mounted = this->mount();
  uint8_t marker = 0x77;
  ASSERT_TRUE(this->backend_.write(DEVICE_SIZE - 8, &marker, 1));

  EXPECT_FALSE(mounted->store.fill(DEVICE_SIZE - 8, 0, 16));
  uint8_t read = 0;
  ASSERT_TRUE(this->backend_.read(DEVICE_SIZE - 8, &read, 1));
  EXPECT_EQ(read, 0x77);
  EXPECT_TRUE(mounted->store.fill(DEVICE_SIZE - 8, 0, 8));
}

TEST_F(FramStoreTest, RamBackendKeepsToItsSize) {
  RamBackend ram{16};
  uint8_t buffer[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_TRUE(ram.is_ready());
  EXPECT_STREQ(ram.type(), "RAM");
  EXPECT_EQ(ram.size(), 16u);
  EXPECT_TRUE(ram.write(8, buffer, sizeof(buffer)));
  EXPECT_FALSE(ram.write(9, buffer, sizeof(buffer)));
  EXPECT_FALSE(ram.write(16, buffer, 1));
  EXPECT_FALSE(ram.read(12, buffer, sizeof(buffer)));
  uint8_t read[8] = {};
  EXPECT_TRUE(ram.read(8, read, sizeof(read)));
  EXPECT_EQ(std::memcmp(read, buffer, sizeof(buffer)), 0);
}

TEST_F(FramStoreTest, DumpsItsConfig) {
  auto mounted = this->mount();
  mounted->store.dump_config();
  EXPECT_TRUE(LogCapture::instance().errors.empty());

  FramStore broken;
  broken.dump_config();
  EXPECT_TRUE(LogCapture::instance().has_error("Store is not available"));
}

}  // namespace esphome::fram_store::testing
