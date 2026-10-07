// Stores by life cycle: several stores on one part, each formatting only its own range. A store above
// another keeps its data through the lower one's format, and nothing outside the stores is touched.

#include "common.h"

namespace esphome::fram_store::testing {
namespace {

static const uint32_t PART_SIZE = 1024;
static const uint16_t LOW_BASE = 0x0100;
static const uint16_t HIGH_BASE = LOW_BASE + SUPERBLOCK_SIZE + 2 * (FramSlot::HEADER_SIZE + SLOT_PAYLOAD);
static const uint32_t SPARE = HIGH_BASE + SUPERBLOCK_SIZE + 2 * (FramSlot::HEADER_SIZE + SLOT_PAYLOAD);
static const uint32_t LOW_HASH = 0x10101010;
static const uint32_t HIGH_HASH = 0x20202020;

// One store of one slot, laid out the way codegen lays it.
struct Store {
  FramStore store;
  FramSlot slot;

  Store(NvramBackend *backend, uint16_t base, uint32_t layout_hash)
      : slot(&store, base + SUPERBLOCK_SIZE, SLOT_PAYLOAD) {
    store.set_backend(backend);
    store.set_expected_size(PART_SIZE);
    store.set_base_offset(base);
    store.set_end(base + SUPERBLOCK_SIZE + slot.size());
    store.set_layout_hash(layout_hash);
    store.add_region(&slot);
    store.setup();
  }
};

class StoresTest : public ::testing::Test {
 protected:
  // Below the stores, the station's bytes; above them, the spare. Both carry a pattern no store writes.
  void SetUp() override {
    LogCapture::instance().clear();
    std::vector<uint8_t> pattern(PART_SIZE);
    for (size_t i = 0; i < pattern.size(); i++)
      pattern[i] = static_cast<uint8_t>(i * 7 + 3);
    ASSERT_TRUE(this->backend_.write(0, pattern.data(), pattern.size()));
    this->outside_ = this->outside();
    this->backend_.guard_below(LOW_BASE);
  }

  std::vector<uint8_t> outside() {
    this->backend_.guard_below(0);
    std::vector<uint8_t> bytes(LOW_BASE + PART_SIZE - SPARE);
    EXPECT_TRUE(this->backend_.read(0, bytes.data(), LOW_BASE));
    EXPECT_TRUE(this->backend_.read(SPARE, bytes.data() + LOW_BASE, PART_SIZE - SPARE));
    this->backend_.guard_below(LOW_BASE);
    return bytes;
  }

  std::unique_ptr<Store> low(uint32_t layout_hash = LOW_HASH) {
    return std::make_unique<Store>(&this->backend_, LOW_BASE, layout_hash);
  }
  std::unique_ptr<Store> high(uint32_t layout_hash = HIGH_HASH) {
    return std::make_unique<Store>(&this->backend_, HIGH_BASE, layout_hash);
  }

  static uint64_t loaded(Store &store) {
    uint64_t value = 0;
    EXPECT_TRUE(store.slot.load(value));
    return value;
  }

  FaultyBackend backend_{PART_SIZE};
  std::vector<uint8_t> outside_;
};

TEST_F(StoresTest, AStoreAboveKeepsItsDataThroughAFormatBelow) {
  uint64_t value = 1;
  ASSERT_TRUE(this->low()->slot.save(value));
  value = 2;
  ASSERT_TRUE(this->high()->slot.save(value));

  ASSERT_TRUE(this->low()->store.format());

  auto low = this->low();
  auto high = this->high();
  EXPECT_FALSE(low->slot.has_data());
  EXPECT_EQ(low->store.format_count(), 2u);
  EXPECT_EQ(high->store.format_count(), 1u);
  EXPECT_EQ(loaded(*high), 2u);
  EXPECT_EQ(this->outside(), this->outside_);
}

TEST_F(StoresTest, AStoreBelowKeepsItsDataThroughAFormatAbove) {
  uint64_t value = 1;
  ASSERT_TRUE(this->low()->slot.save(value));
  value = 2;
  ASSERT_TRUE(this->high()->slot.save(value));

  ASSERT_TRUE(this->high()->store.format());

  EXPECT_EQ(loaded(*this->low()), 1u);
  EXPECT_FALSE(this->high()->slot.has_data());
  EXPECT_EQ(this->outside(), this->outside_);
}

// A changed layout empties its own store and no other.
TEST_F(StoresTest, ALayoutChangeEmptiesOnlyItsOwnStore) {
  uint64_t value = 1;
  ASSERT_TRUE(this->low()->slot.save(value));
  value = 2;
  ASSERT_TRUE(this->high()->slot.save(value));

  auto changed = this->low(0x30303030);
  EXPECT_FALSE(changed->slot.has_data());
  EXPECT_TRUE(LogCapture::instance().has_warning("Formatting 64 bytes at 0x0100-0x013F"));
  EXPECT_EQ(loaded(*this->high()), 2u);
  EXPECT_EQ(this->outside(), this->outside_);
}

// Build A has the lower store; build B adds the upper one in A's spare. Booting A, B, A and B again --
// a rollback and a roll forward -- wipes nothing.
TEST_F(StoresTest, ARollbackBetweenBuildsThatOnlyAddStoresWipesNothing) {
  uint64_t value = 1;
  ASSERT_TRUE(this->low()->slot.save(value));  // A

  {
    auto low = this->low();  // B
    auto high = this->high();
    EXPECT_EQ(loaded(*low), 1u);
    value = 2;
    ASSERT_TRUE(high->slot.save(value));
  }

  {
    auto low = this->low();  // A again
    EXPECT_EQ(low->store.format_count(), 1u);
    value = 3;
    ASSERT_TRUE(low->slot.save(value));
  }

  auto low = this->low();  // B again
  auto high = this->high();
  EXPECT_EQ(loaded(*low), 3u);
  EXPECT_EQ(loaded(*high), 2u);
  EXPECT_EQ(low->store.format_count(), 1u);
  EXPECT_EQ(high->store.format_count(), 1u);
  EXPECT_EQ(this->outside(), this->outside_);
}

// A region's helpers in reach, to aim them past its own end.
class ProbeRegion : public FramRegion {
 public:
  using FramRegion::FramRegion;
  void init() override { this->initialised_ = true; }
  const char *kind() const override { return "probe"; }
  using FramRegion::fill_;
  using FramRegion::read_;
  using FramRegion::write_;
};

// The store keeps a region inside the store; the region keeps itself off its neighbour.
TEST_F(StoresTest, ARegionKeepsToItsOwnBytes) {
  static const uint16_t FIRST = LOW_BASE + SUPERBLOCK_SIZE;
  FramStore store;
  ProbeRegion probe(&store, FIRST, 16);
  ProbeRegion neighbour(&store, FIRST + 16, 16);
  store.set_backend(&this->backend_);
  store.set_expected_size(PART_SIZE);
  store.set_base_offset(LOW_BASE);
  store.set_end(FIRST + 32);
  store.set_layout_hash(LOW_HASH);
  store.add_region(&probe);
  store.add_region(&neighbour);
  store.setup();
  ASSERT_TRUE(store.is_ready());

  const uint8_t marker[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  ASSERT_TRUE(neighbour.write_(0, marker, sizeof(marker)));

  uint8_t buffer[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_TRUE(probe.write_(8, buffer, sizeof(buffer)));
  EXPECT_FALSE(probe.write_(12, buffer, sizeof(buffer)));
  EXPECT_TRUE(LogCapture::instance().has_error("8 bytes at +12 run past the 16-byte region at 0x0120"));
  EXPECT_FALSE(probe.read_(16, buffer, 1));
  EXPECT_FALSE(probe.fill_(0, 0, 17));
  EXPECT_FALSE(probe.read_(UINT32_MAX, buffer, 1));

  uint8_t read[8] = {};
  ASSERT_TRUE(neighbour.read_(0, read, sizeof(read)));
  EXPECT_EQ(std::memcmp(read, marker, sizeof(marker)), 0);
}

}  // namespace
}  // namespace esphome::fram_store::testing
