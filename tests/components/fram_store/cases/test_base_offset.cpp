// base_offset: the bytes below it are someone else's (the factory station's, on the CPU board),
// so the store must place itself above them and never read or write them, through a format too.

#include "common.h"

namespace esphome::fram_store::testing {
namespace {

static const uint32_t PART_SIZE = 1024;
static const uint16_t BASE = 0x200;
static const uint16_t BASE_SLOT = BASE + SUPERBLOCK_SIZE;
static const uint16_t BASE_RING = BASE_SLOT + 2 * (FramSlot::HEADER_SIZE + SLOT_PAYLOAD);

class BaseOffsetTest : public ::testing::Test {
 protected:
  struct Mounted {
    FramStore store;
    FramSlot slot;
    FramRing ring;

    Mounted(NvramBackend *backend, uint16_t base)
        : slot(&store, base + SUPERBLOCK_SIZE, SLOT_PAYLOAD),
          ring(&store, base + SUPERBLOCK_SIZE + 2 * (FramSlot::HEADER_SIZE + SLOT_PAYLOAD), RING_RECORD,
               RING_CAPACITY) {
      store.set_backend(backend);
      store.set_expected_size(PART_SIZE);
      store.set_base_offset(base);
      store.set_layout_hash(LAYOUT_HASH);
      store.add_region(&slot);
      store.add_region(&ring);
      store.setup();
    }
  };

  // What a factory station leaves below the store: text at 0x0000 and 0x0100, a pattern elsewhere.
  void SetUp() override {
    LogCapture::instance().clear();
    for (uint16_t address = 0; address < BASE; address++) {
      uint8_t byte = static_cast<uint8_t>(address * 13 + 1);
      ASSERT_TRUE(this->backend_.write(address, &byte, 1));
    }
    ASSERT_TRUE(this->backend_.write(0x0000, reinterpret_cast<const uint8_t *>("JetHome"), 7));
    ASSERT_TRUE(this->backend_.write(0x0100, reinterpret_cast<const uint8_t *>("JH_TEST!"), 8));
    this->station_ = this->snapshot_below_base();
    this->backend_.guard_below(BASE);
  }

  std::vector<uint8_t> snapshot_below_base() {
    this->backend_.guard_below(0);
    std::vector<uint8_t> bytes(BASE);
    EXPECT_TRUE(this->backend_.read(0, bytes.data(), bytes.size()));
    this->backend_.guard_below(BASE);
    return bytes;
  }

  std::unique_ptr<Mounted> mount(uint16_t base = BASE) { return std::make_unique<Mounted>(&this->backend_, base); }

  FaultyBackend backend_{PART_SIZE};
  std::vector<uint8_t> station_;
};

TEST_F(BaseOffsetTest, PlacesTheSuperblockAtTheBase) {
  auto mounted = this->mount();
  ASSERT_TRUE(mounted->store.is_ready());
  EXPECT_EQ(mounted->store.base_offset(), BASE);
  EXPECT_EQ(mounted->store.format_count(), 1u);
  EXPECT_TRUE(LogCapture::instance().has_warning("Formatting 512 bytes from 0x0200"));

  uint8_t magic[4] = {};
  ASSERT_TRUE(this->backend_.read(BASE, magic, sizeof(magic)));
  EXPECT_EQ(memcmp(magic, "JFRM", sizeof(magic)), 0);
  EXPECT_EQ(this->snapshot_below_base(), this->station_);
}

TEST_F(BaseOffsetTest, KeepsWhatIsBelowTheBaseThroughEverything) {
  {
    auto mounted = this->mount();
    uint64_t value = 1;
    ASSERT_TRUE(mounted->slot.save(value));
    for (value = 1; value <= RING_CAPACITY + 3; value++) {
      ASSERT_TRUE(mounted->ring.append(value));
    }
    ASSERT_TRUE(mounted->slot.clear());
    ASSERT_TRUE(mounted->ring.clear());
    ASSERT_TRUE(mounted->store.format());
    value = 42;
    ASSERT_TRUE(mounted->slot.save(value));
  }

  auto restarted = this->mount();
  EXPECT_EQ(restarted->store.format_count(), 2u);
  uint64_t read = 0;
  ASSERT_TRUE(restarted->slot.load(read));
  EXPECT_EQ(read, 42u);
  EXPECT_EQ(this->snapshot_below_base(), this->station_);
}

TEST_F(BaseOffsetTest, RefusesAccessBelowTheBase) {
  auto mounted = this->mount();
  uint8_t buffer[8] = {};

  EXPECT_FALSE(mounted->store.read(0x0000, buffer, sizeof(buffer)));
  EXPECT_FALSE(mounted->store.write(0x0100, buffer, sizeof(buffer)));
  EXPECT_FALSE(mounted->store.fill(0x0000, 0, BASE));
  // A span that starts below the base and ends above it is refused whole.
  EXPECT_FALSE(mounted->store.write(BASE - 4, buffer, sizeof(buffer)));
  EXPECT_FALSE(mounted->store.fill(BASE - 4, 0, sizeof(buffer)));

  EXPECT_TRUE(mounted->store.read(BASE, buffer, sizeof(buffer)));
  EXPECT_EQ(this->snapshot_below_base(), this->station_);
}

TEST_F(BaseOffsetTest, RegionsLiveAboveTheSuperblock) {
  auto mounted = this->mount();
  EXPECT_EQ(mounted->slot.offset(), BASE_SLOT);
  EXPECT_EQ(mounted->ring.offset(), BASE_RING);

  uint64_t value = 0x1122334455667788;
  ASSERT_TRUE(mounted->slot.save(value));
  uint64_t raw = 0;
  ASSERT_TRUE(this->backend_.read(BASE_SLOT + FramSlot::HEADER_SIZE, reinterpret_cast<uint8_t *>(&raw), sizeof(raw)));
  EXPECT_EQ(raw, value);
}

// The superblock is looked for at the base: a store moved up finds none there and starts over.
TEST_F(BaseOffsetTest, AStoreMovedUpStartsOver) {
  this->backend_.guard_below(0);
  {
    auto low = this->mount(0);
    uint64_t value = 5;
    ASSERT_TRUE(low->slot.save(value));
  }

  auto high = this->mount(BASE);
  EXPECT_TRUE(high->store.is_ready());
  EXPECT_EQ(high->store.format_count(), 1u);
  EXPECT_FALSE(high->slot.has_data());
}

TEST_F(BaseOffsetTest, RefusesABaseWithNoRoomForTheSuperblock) {
  FramStore store;
  store.set_backend(&this->backend_);
  store.set_expected_size(PART_SIZE);
  store.set_base_offset(PART_SIZE - SUPERBLOCK_SIZE / 2);
  store.setup();

  EXPECT_FALSE(store.is_ready());
  EXPECT_TRUE(store.is_failed());
  EXPECT_TRUE(LogCapture::instance().has_error("No room for the superblock at 0x03F0"));
  EXPECT_EQ(this->snapshot_below_base(), this->station_);
}

}  // namespace
}  // namespace esphome::fram_store::testing
