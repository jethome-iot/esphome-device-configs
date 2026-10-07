// Power lost part way through an operation: the part keeps the first k bytes of what was sent and nothing
// after. For every k, the next boot has to find the state before the operation or the one after it, never
// an error and never bytes nobody saved.

#include "common.h"

namespace esphome::fram_store::testing {
namespace {

class PowerCutBackend : public RamBackend {
 public:
  using RamBackend::RamBackend;

  void cut_after(size_t bytes) {
    this->budget_ = bytes;
    this->armed_ = true;
  }
  void restore() { this->armed_ = false; }
  size_t written() const { return this->written_; }

  bool write(uint16_t address, const uint8_t *data, size_t len) override {
    const size_t take = this->armed_ ? std::min(len, this->budget_) : len;
    if (this->armed_)
      this->budget_ -= take;
    this->written_ += take;
    if (take != 0 && !RamBackend::write(address, data, take))
      return false;
    return take == len;
  }

 protected:
  size_t budget_{0};
  size_t written_{0};
  bool armed_{false};
};

// Runs `act` once to learn how many bytes it writes, then once per prefix of that on a part `prepare` set up
// the same way, and hands the boot after each cut to `check` with how far the operation got.
template<typename Prepare, typename Act, typename Check> void every_cut(Prepare prepare, Act act, Check check) {
  size_t total = 0;
  {
    PowerCutBackend part(DEVICE_SIZE);
    prepare(part);
    Mounted mounted(&part);
    const size_t before = part.written();
    act(mounted);
    total = part.written() - before;
  }
  ASSERT_GT(total, 0u);

  for (size_t k = 0; k <= total; k++) {
    SCOPED_TRACE(::testing::Message() << "power cut after " << k << " of " << total << " bytes");
    PowerCutBackend part(DEVICE_SIZE);
    prepare(part);
    {
      Mounted mounted(&part);
      part.cut_after(k);
      act(mounted);
    }
    part.restore();
    Mounted next(&part);
    ASSERT_TRUE(next.store.is_ready());
    ASSERT_TRUE(next.slot.is_initialised());
    ASSERT_TRUE(next.ring.is_initialised());
    check(next, k, total);
  }
}

// The slot's saves before the cut, oldest first; the last is its value.
auto saved(std::vector<uint64_t> values) {
  return [values](PowerCutBackend &part) {
    Mounted mounted(&part);
    for (uint64_t value : values)
      ASSERT_TRUE(mounted.slot.save(value));
  };
}

auto appended(uint64_t count) {
  return [count](PowerCutBackend &part) {
    Mounted mounted(&part);
    for (uint64_t value = 1; value <= count; value++)
      ASSERT_TRUE(mounted.ring.append(value));
  };
}

// What the slot holds, or 0 when it holds nothing; no test saves a 0.
uint64_t slot_value(Mounted &mounted) {
  uint64_t value = 0;
  if (!mounted.slot.load(value)) {
    EXPECT_FALSE(mounted.slot.has_data()) << "a slot that claims data has to load it";
    return 0;
  }
  return value;
}

// Whatever the cut left, the slot takes the next save and gives it back after a reboot.
void expect_slot_works(Mounted &mounted, PowerCutBackend &part) {
  uint64_t value = 0xC0FFEE;
  ASSERT_TRUE(mounted.slot.save(value));
  Mounted again(&part);
  value = 0;
  ASSERT_TRUE(again.slot.load(value));
  EXPECT_EQ(value, 0xC0FFEEu);
}

class PowerCutTest : public ::testing::Test {
 protected:
  void SetUp() override { LogCapture::instance().clear(); }

  void slot_save_from(std::vector<uint64_t> before) {
    const uint64_t old = before.empty() ? 0 : before.back();
    const uint64_t fresh = 0xBBBB;
    every_cut(
        saved(before), [fresh](Mounted &mounted) { mounted.slot.save(fresh); },
        [old, fresh](Mounted &next, size_t k, size_t total) {
          const uint64_t value = slot_value(next);
          EXPECT_TRUE(value == old || value == fresh) << "slot holds " << value;
          if (k == 0)
            EXPECT_EQ(value, old);
          if (k == total)
            EXPECT_EQ(value, fresh);
        });
  }

  void slot_clear_from(std::vector<uint64_t> before) {
    const uint64_t old = before.back();
    every_cut(
        saved(before), [](Mounted &mounted) { mounted.slot.clear(); },
        [old](Mounted &next, size_t k, size_t total) {
          const uint64_t value = slot_value(next);
          EXPECT_TRUE(value == old || value == 0) << "slot holds " << value;
          if (k == 0)
            EXPECT_EQ(value, old);
          if (k == total)
            EXPECT_EQ(value, 0u);
        });
  }
};

TEST_F(PowerCutTest, ASaveIntoAnEmptySlot) { this->slot_save_from({}); }
TEST_F(PowerCutTest, ASaveNextToOneCopy) { this->slot_save_from({0x1111}); }
TEST_F(PowerCutTest, ASaveOverTheFirstCopy) { this->slot_save_from({0x1111, 0x2222}); }
TEST_F(PowerCutTest, ASaveOverTheSecondCopy) { this->slot_save_from({0x1111, 0x2222, 0x3333}); }

// The older copy is wiped first in both cases: the other order would bring the value before back.
TEST_F(PowerCutTest, AClearWithTheSecondCopyCurrent) { this->slot_clear_from({0x1111, 0x2222}); }
TEST_F(PowerCutTest, AClearWithTheFirstCopyCurrent) { this->slot_clear_from({0x1111, 0x2222, 0x3333}); }

TEST_F(PowerCutTest, ASlotTakesASaveAfterEveryCut) {
  for (size_t k = 0; k < 2 * (FramSlot::HEADER_SIZE + SLOT_PAYLOAD); k++) {
    SCOPED_TRACE(k);
    PowerCutBackend part(DEVICE_SIZE);
    saved({0x1111, 0x2222})(part);
    {
      Mounted mounted(&part);
      part.cut_after(k);
      mounted.slot.save(uint64_t{0x3333});
      mounted.slot.clear();
    }
    part.restore();
    Mounted next(&part);
    expect_slot_works(next, part);
  }
}

// Into a blank record: the ring holds the records before the append, or those and the new one.
TEST_F(PowerCutTest, AnAppendToARingThatIsNotFull) {
  every_cut(
      appended(2), [](Mounted &mounted) { mounted.ring.append(uint64_t{3}); },
      [](Mounted &next, size_t k, size_t total) {
        const uint16_t count = next.ring.count();
        EXPECT_TRUE(count == 2 || count == 3) << "ring holds " << count;
        if (k == 0)
          EXPECT_EQ(count, 2u);
        if (k == total)
          EXPECT_EQ(count, 3u);
        for (uint16_t index = 0; index < count; index++) {
          uint64_t value = 0;
          ASSERT_TRUE(next.ring.peek(index, value)) << "index " << index;
          EXPECT_EQ(value, count - index);
        }
      });
}

// Over the oldest record: that record is gone either way, so it may read as missing; every other record
// is the one that was there, and the newest is the append or the one before it.
TEST_F(PowerCutTest, AnAppendToAFullRing) {
  const uint64_t before = RING_CAPACITY + 1;
  every_cut(
      appended(before), [before](Mounted &mounted) { mounted.ring.append(before + 1); },
      [before](Mounted &next, size_t k, size_t total) {
        uint64_t newest = 0;
        ASSERT_TRUE(next.ring.peek(0, newest));
        EXPECT_TRUE(newest == before || newest == before + 1) << "newest is " << newest;
        if (k == 0)
          EXPECT_EQ(newest, before);
        if (k == total)
          EXPECT_EQ(newest, before + 1);
        EXPECT_GE(next.ring.count(), RING_CAPACITY - 1);
        for (uint16_t index = 0; index < next.ring.count(); index++) {
          uint64_t value = 0;
          if (next.ring.peek(index, value)) {
            EXPECT_EQ(value, newest - index) << "index " << index;
          } else {
            EXPECT_TRUE(newest == before && index == RING_CAPACITY - 1) << "index " << index << " is missing";
          }
        }
      });
}

// Oldest first: a cut leaves the newest records, with no gap among them.
void ring_clear_from(uint64_t appends) {
  const uint16_t old_count = std::min<uint64_t>(appends, RING_CAPACITY);
  every_cut(
      appended(appends), [](Mounted &mounted) { mounted.ring.clear(); },
      [appends, old_count](Mounted &next, size_t k, size_t total) {
        const uint16_t count = next.ring.count();
        EXPECT_LE(count, old_count);
        if (k == 0)
          EXPECT_EQ(count, old_count);
        if (k == total)
          EXPECT_EQ(count, 0u);
        for (uint16_t index = 0; index < count; index++) {
          uint64_t value = 0;
          ASSERT_TRUE(next.ring.peek(index, value)) << "index " << index;
          EXPECT_EQ(value, appends - index);
        }
      });
}

TEST_F(PowerCutTest, AClearOfARingThatIsNotFull) { ring_clear_from(3); }
TEST_F(PowerCutTest, AClearOfAWrappedRing) { ring_clear_from(RING_CAPACITY + 2); }

// The superblock goes first and comes back last: the store is as it was, or empty, never half of each.
TEST_F(PowerCutTest, AFormat) {
  every_cut(
      [](PowerCutBackend &part) {
        Mounted mounted(&part);
        ASSERT_TRUE(mounted.slot.save(uint64_t{0x1111}));
        for (uint64_t value = 1; value <= 3; value++)
          ASSERT_TRUE(mounted.ring.append(value));
      },
      [](Mounted &mounted) { mounted.store.format(); },
      [](Mounted &next, size_t k, size_t total) {
        const uint64_t value = slot_value(next);
        const uint16_t count = next.ring.count();
        const bool old = value == 0x1111 && count == 3;
        const bool fresh = value == 0 && count == 0;
        EXPECT_TRUE(old || fresh) << "slot holds " << value << ", ring " << count;
        if (k == 0)
          EXPECT_TRUE(old);
        if (k == total)
          EXPECT_TRUE(fresh);
        for (uint16_t index = 0; index < count; index++) {
          uint64_t record = 0;
          ASSERT_TRUE(next.ring.peek(index, record));
          EXPECT_EQ(record, 3u - index);
        }
      });
}

}  // namespace
}  // namespace esphome::fram_store::testing
