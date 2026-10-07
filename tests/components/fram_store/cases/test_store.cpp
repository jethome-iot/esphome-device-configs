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
  EXPECT_TRUE(
      LogCapture::instance().has_warning("No superblock at 0x0000: the range is blank or holds something else"));
  EXPECT_TRUE(LogCapture::instance().has_warning("Formatting 120 bytes at 0x0000-0x0077"));
}

// The format and every access stop at the end codegen passes: the rest of the part is other stores' or spare.
TEST_F(FramStoreTest, KeepsToItsOwnRange) {
  std::vector<uint8_t> beyond(DEVICE_SIZE - STORE_END, 0x5A);
  ASSERT_TRUE(this->backend_.write(STORE_END, beyond.data(), beyond.size()));

  auto mounted = this->mount();
  EXPECT_EQ(mounted->store.end(), STORE_END);
  uint64_t value = 1;
  ASSERT_TRUE(mounted->slot.save(value));
  ASSERT_TRUE(mounted->store.format());

  uint8_t buffer[8] = {};
  EXPECT_FALSE(mounted->store.read(STORE_END, buffer, 1));
  EXPECT_FALSE(mounted->store.write(STORE_END - 4, buffer, sizeof(buffer)));
  EXPECT_FALSE(mounted->store.fill(STORE_END - 4, 0, sizeof(buffer)));
  EXPECT_TRUE(mounted->store.read(STORE_END - 8, buffer, sizeof(buffer)));

  std::vector<uint8_t> after(beyond.size());
  ASSERT_TRUE(this->backend_.read(STORE_END, after.data(), after.size()));
  EXPECT_EQ(after, beyond);
}

TEST_F(FramStoreTest, KeepsTheSuperblockAcrossRestarts) {
  EXPECT_EQ(this->mount()->store.format_count(), 1u);
  EXPECT_EQ(this->mount()->store.format_count(), 1u);
}

TEST_F(FramStoreTest, ReformatsWhenTheLayoutChanged) {
  EXPECT_EQ(this->mount()->store.format_count(), 1u);
  EXPECT_EQ(this->mount(0x99999999)->store.format_count(), 2u);
  EXPECT_TRUE(LogCapture::instance().has_warning("Layout at 0x0000 changed: hash 0xABCD1234 -> 0x99999999"));
}

// A transient NACK on the shared bus must not read as "blank device" and wipe live data.
TEST_F(FramStoreTest, DoesNotFormatWhenTheSuperblockCannotBeRead) {
  uint64_t written = 99;
  ASSERT_TRUE(this->mount()->slot.save(written));

  this->backend_.fail_reads_covering(0);
  auto failed = this->mount();
  EXPECT_FALSE(failed->store.is_ready());
  EXPECT_TRUE(LogCapture::instance().has_error("Superblock at 0x0000 could not be read, refusing to touch the store"));

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

// init() proves the copy was good at boot; load() has to prove it is still good. With nothing to fall back
// to, the slot then refuses saves: a consumer starting over from zero would overwrite both copies.
TEST_F(FramStoreTest, SlotLoadRejectsACopyDamagedAfterInit) {
  auto mounted = this->mount();
  uint64_t value = 7;
  ASSERT_TRUE(mounted->slot.save(value));

  this->corrupt(slot_payload(0));

  uint64_t read = 0;
  EXPECT_FALSE(mounted->slot.load(read));
  EXPECT_TRUE(mounted->slot.load_failed());
  EXPECT_TRUE(LogCapture::instance().has_error(
      "Slot at 0x0020 could not be loaded; saving is refused until a load succeeds or clear()"));

  uint64_t fresh = 0;
  EXPECT_FALSE(mounted->slot.save(fresh));
  EXPECT_TRUE(LogCapture::instance().has_error("Slot at 0x0020 failed its last load, refusing to save over it"));

  // clear() is the consumer saying it starts over.
  ASSERT_TRUE(mounted->slot.clear());
  EXPECT_FALSE(mounted->slot.load_failed());
  ASSERT_TRUE(mounted->slot.save(fresh));
  ASSERT_TRUE(mounted->slot.load(read));
  EXPECT_EQ(read, 0u);
}

// The boot-time rule, applied at load time: a damaged newest copy gives way to the one before it.
TEST_F(FramStoreTest, SlotLoadFallsBackWhenTheNewestCopyIsDamagedAfterInit) {
  auto mounted = this->mount();
  uint64_t value = 111;
  ASSERT_TRUE(mounted->slot.save(value));
  value = 222;
  ASSERT_TRUE(mounted->slot.save(value));
  const uint32_t newest = mounted->slot.seq();

  this->corrupt(slot_payload(1));

  uint64_t read = 0;
  ASSERT_TRUE(mounted->slot.load(read));
  EXPECT_EQ(read, 111u);
  EXPECT_FALSE(mounted->slot.load_failed());
  EXPECT_LT(mounted->slot.seq(), newest);
  EXPECT_TRUE(
      LogCapture::instance().has_warning("Slot at 0x0020: the newest copy is damaged, loaded the one before it"));

  // The next save takes the damaged copy and numbers above it, so it wins the next boot.
  value = 333;
  ASSERT_TRUE(mounted->slot.save(value));
  EXPECT_GT(mounted->slot.seq(), newest);
  ASSERT_TRUE(mounted->slot.load(read));
  EXPECT_EQ(read, 333u);

  auto restarted = this->mount();
  ASSERT_TRUE(restarted->slot.load(read));
  EXPECT_EQ(read, 333u);
}

// An unreadable newest copy may be intact: loading the older one would hand out a stale value as current.
TEST_F(FramStoreTest, SlotLoadDoesNotFallBackPastAnUnreadableCopy) {
  auto mounted = this->mount();
  uint64_t value = 111;
  ASSERT_TRUE(mounted->slot.save(value));
  value = 222;
  ASSERT_TRUE(mounted->slot.save(value));

  this->backend_.fail_reads_covering(slot_payload(1));
  uint64_t read = 0;
  EXPECT_FALSE(mounted->slot.load(read));
  EXPECT_TRUE(mounted->slot.load_failed());
  value = 0;
  EXPECT_FALSE(mounted->slot.save(value));

  // Once the part answers, the newest value is there and saving is allowed again.
  this->backend_.stop_failing();
  ASSERT_TRUE(mounted->slot.load(read));
  EXPECT_EQ(read, 222u);
  EXPECT_FALSE(mounted->slot.load_failed());
  value = 333;
  ASSERT_TRUE(mounted->slot.save(value));
}

// The header read is the first one a load makes; failing it is no more a verdict than failing the payload.
TEST_F(FramStoreTest, SlotLoadFailsWhenAHeaderCannotBeRead) {
  auto mounted = this->mount();
  uint64_t value = 5;
  ASSERT_TRUE(mounted->slot.save(value));

  this->backend_.fail_reads_covering(slot_copy(0), 1);
  uint64_t read = 0;
  EXPECT_FALSE(mounted->slot.load(read));
  ASSERT_TRUE(mounted->slot.load(read));
  EXPECT_EQ(read, 5u);
}

// An empty slot has nothing to protect: the first boot loads nothing and saves.
TEST_F(FramStoreTest, AnEmptySlotLoadsNothingAndStillSaves) {
  auto mounted = this->mount();
  uint64_t value = 0;
  EXPECT_FALSE(mounted->slot.load(value));
  EXPECT_FALSE(mounted->slot.load_failed());
  value = 9;
  EXPECT_TRUE(mounted->slot.save(value));
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

// The end codegen passes is checked against the part before anything is read.
TEST_F(FramStoreTest, RefusesAStoreThatRunsPastTheDevice) {
  FramStore store;
  store.set_backend(&this->backend_);
  store.set_expected_size(DEVICE_SIZE);
  store.set_base_offset(0x100);
  store.set_end(DEVICE_SIZE + 4);
  store.setup();

  EXPECT_FALSE(store.is_ready());
  EXPECT_TRUE(store.is_failed());
  EXPECT_TRUE(LogCapture::instance().has_error("Store at 0x0100 runs to 0x0203, past the 512-byte device"));
}

// A store built without an end has no room for anything, not the whole part.
TEST_F(FramStoreTest, RefusesAStoreWithoutAnEnd) {
  FramStore store;
  store.set_backend(&this->backend_);
  store.set_expected_size(DEVICE_SIZE);
  store.setup();

  EXPECT_FALSE(store.is_ready());
  EXPECT_TRUE(LogCapture::instance().has_error("No room for the superblock at 0x0000"));
}

// A region on the superblock or past the end would write over the store's own header or a neighbour.
TEST_F(FramStoreTest, RefusesARegionOutsideTheStore) {
  struct Case {
    uint16_t offset;
    const char *error;
  };
  for (const Case &bad : {Case{SUPERBLOCK_SIZE - 4, "Region at 0x001C (32 bytes) is outside the store's 0x0020-0x0077"},
                          Case{STORE_END - 16, "Region at 0x0068 (32 bytes) is outside the store's 0x0020-0x0077"}}) {
    SCOPED_TRACE(bad.error);
    LogCapture::instance().clear();
    FramStore store;
    FramSlot slot(&store, bad.offset, SLOT_PAYLOAD);
    store.set_backend(&this->backend_);
    store.set_expected_size(DEVICE_SIZE);
    store.set_end(STORE_END);
    store.set_layout_hash(LAYOUT_HASH);
    store.add_region(&slot);
    store.setup();

    EXPECT_FALSE(store.is_ready());
    EXPECT_FALSE(slot.is_initialised());
    EXPECT_TRUE(LogCapture::instance().has_error(bad.error));
  }
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
  store.set_end(STORE_END);
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
  EXPECT_TRUE(LogCapture::instance().has_warning("Superblock at 0x0000 fails its CRC"));
}

TEST_F(FramStoreTest, ReformatsWhenTheMagicIsWrong) {
  ASSERT_TRUE(this->mount()->store.is_ready());
  uint8_t magic[4] = {'X', 'X', 'X', 'X'};
  ASSERT_TRUE(this->backend_.write(0, magic, sizeof(magic)));
  LogCapture::instance().clear();
  EXPECT_EQ(this->mount()->store.format_count(), 1u);
  EXPECT_TRUE(LogCapture::instance().has_warning("No superblock at 0x0000"));
}

// Another version may lay its header out differently, so nothing in it is trusted, the count included.
TEST_F(FramStoreTest, ReformatsWhenTheSuperblockIsAnotherVersion) {
  ASSERT_TRUE(this->mount()->store.is_ready());

  uint8_t block[SUPERBLOCK_SIZE];
  ASSERT_TRUE(this->backend_.read(0, block, sizeof(block)));
  const uint16_t version = 2;
  std::memcpy(block + 4, &version, sizeof(version));
  const uint16_t crc = crc16(block, sizeof(block) - 2);
  std::memcpy(block + sizeof(block) - 2, &crc, sizeof(crc));
  ASSERT_TRUE(this->backend_.write(0, block, sizeof(block)));

  EXPECT_EQ(this->mount()->store.format_count(), 1u);
  EXPECT_TRUE(LogCapture::instance().has_warning(
      "Superblock at 0x0000 is version 2 of 32 bytes, this firmware reads version 1 of 32"));
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
  EXPECT_TRUE(
      LogCapture::instance().has_warning("Superblock at 0x0000 was written for a 1024-byte device, this one has 512"));
}

TEST_F(FramStoreTest, FillRefusesASpanPastTheEndWhole) {
  auto mounted = this->mount();
  uint8_t marker = 0x77;
  ASSERT_TRUE(this->backend_.write(STORE_END - 8, &marker, 1));

  EXPECT_FALSE(mounted->store.fill(STORE_END - 8, 0, 16));
  uint8_t read = 0;
  ASSERT_TRUE(this->backend_.read(STORE_END - 8, &read, 1));
  EXPECT_EQ(read, 0x77);
  EXPECT_TRUE(mounted->store.fill(STORE_END - 8, 0, 8));
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
