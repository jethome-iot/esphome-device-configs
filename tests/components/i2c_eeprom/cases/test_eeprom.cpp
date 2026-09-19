// The component over a bus that records every transaction: how a memory address is framed for
// each part size, where a write is split into page-sized transactions, and what a chip that
// does not answer does to it.

#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "esphome/components/i2c_eeprom/i2c_eeprom.h"
#include "esphome/core/automation.h"
#include "esphome/core/base_automation.h"

namespace esphome::i2c_eeprom::testing {
namespace {

struct Transaction {
  uint8_t address;
  std::vector<uint8_t> written;
  size_t read;
};

// One chip whose memory address is `address_bytes` wide; reads stream from its pointer, a
// longer write stores the bytes that follow the address — inside one `page`, rolling over to
// that page's first byte as a 24Cxx does instead of carrying into the next page.
class RecordingBus : public i2c::I2CBus {
 public:
  std::vector<Transaction> log;
  std::vector<uint8_t> image;
  uint8_t present_at{0x54};
  size_t address_bytes{2};
  size_t page{8};
  bool refuse_reads{false};
  // The write carrying data with this index (0-based) gets a bus error and stores nothing.
  int refuse_write{-1};

  RecordingBus() : image(8192) {
    for (size_t i = 0; i < this->image.size(); i++)
      this->image[i] = static_cast<uint8_t>(i * 7);
  }

  i2c::ErrorCode write_readv(uint8_t addr, const uint8_t *write_buffer, size_t write_count, uint8_t *read_buffer,
                             size_t read_count) override {
    this->log.push_back({addr, std::vector<uint8_t>(write_buffer, write_buffer + write_count), read_count});
    if (addr != this->present_at)
      return i2c::ERROR_NOT_ACKNOWLEDGED;
    if (write_count > this->address_bytes && this->data_writes_++ == this->refuse_write)
      return i2c::ERROR_TIMEOUT;
    if (write_count >= this->address_bytes) {
      this->pointer_ = 0;
      for (size_t i = 0; i < this->address_bytes; i++)
        this->pointer_ = (this->pointer_ << 8) | write_buffer[i];
      size_t offset = this->pointer_ % this->page;
      const size_t base = this->pointer_ - offset;
      for (size_t i = this->address_bytes; i < write_count; i++) {
        this->image.at(base + offset) = write_buffer[i];
        offset = (offset + 1) % this->page;
      }
      this->pointer_ = base + offset;
    }
    if (read_count != 0 && this->refuse_reads)
      return i2c::ERROR_TIMEOUT;
    for (size_t i = 0; i < read_count; i++)
      read_buffer[i] = this->image.at(this->pointer_++);
    return i2c::ERROR_OK;
  }

 protected:
  size_t pointer_{0};
  int data_writes_{0};
};

struct Chip {
  RecordingBus bus;
  I2CEeprom eeprom;

  explicit Chip(uint32_t size_bytes, uint16_t page_size = 8) {
    this->eeprom.set_i2c_bus(&this->bus);
    this->eeprom.set_i2c_address(0x54);
    this->eeprom.set_size(size_bytes);
    this->eeprom.set_page_size(page_size);
    this->bus.address_bytes = size_bytes > 2048 ? 2 : 1;
    this->bus.page = page_size;
  }
};

// Bytes a test can tell apart from the pattern the image is filled with.
std::vector<uint8_t> pattern(size_t size) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; i++)
    data[i] = static_cast<uint8_t>(0xA0 + i);
  return data;
}

TEST(I2CEeprom, ReadsA64KbitPartThroughATwoByteAddress) {
  Chip chip(8192);
  uint8_t buf[4];
  ASSERT_TRUE(chip.eeprom.get(0x0123, buf, sizeof(buf)));

  ASSERT_EQ(chip.bus.log.size(), 2u);
  EXPECT_EQ(chip.bus.log[0].address, 0x54);
  EXPECT_EQ(chip.bus.log[0].written, (std::vector<uint8_t>{0x01, 0x23}));
  EXPECT_EQ(chip.bus.log[0].read, 0u);
  EXPECT_TRUE(chip.bus.log[1].written.empty());
  EXPECT_EQ(chip.bus.log[1].read, 4u);
  EXPECT_EQ(memcmp(buf, chip.bus.image.data() + 0x123, sizeof(buf)), 0);
}

TEST(I2CEeprom, ReadsA16KbitPartThroughAOneByteAddress) {
  Chip chip(2048);
  uint8_t byte = 0;
  ASSERT_TRUE(chip.eeprom.get(0x45, &byte));

  ASSERT_EQ(chip.bus.log.size(), 2u);
  EXPECT_EQ(chip.bus.log[0].written, (std::vector<uint8_t>{0x45}));
  EXPECT_EQ(chip.bus.log[1].read, 1u);
  EXPECT_EQ(byte, chip.bus.image[0x45]);
}

// 16 Kbit (2048 bytes) is the last one-byte part; 32 Kbit takes two.
TEST(I2CEeprom, TheAddressWidthChangesAbove16Kbit) {
  Chip one(2048);
  uint8_t byte = 0;
  ASSERT_TRUE(one.eeprom.get(0x10, &byte));
  EXPECT_EQ(one.bus.log[0].written.size(), 1u);

  Chip two(4096);
  ASSERT_TRUE(two.eeprom.get(0x10, &byte));
  EXPECT_EQ(two.bus.log[0].written, (std::vector<uint8_t>{0x00, 0x10}));
}

// The whole address space of the largest part, and the last byte's two-byte frame.
TEST(I2CEeprom, ReachesTheLastByteOfA512KbitPart) {
  Chip chip(65536);
  chip.bus.image.resize(65536, 0x99);
  uint8_t byte = 0;
  ASSERT_TRUE(chip.eeprom.get(0xFFFF, &byte));
  EXPECT_EQ(chip.bus.log[0].written, (std::vector<uint8_t>{0xFF, 0xFF}));
  EXPECT_EQ(byte, 0x99);
}

TEST(I2CEeprom, RefusesARangeThatDoesNotFitThePart) {
  Chip chip(8192);
  uint8_t buf[4];
  EXPECT_FALSE(chip.eeprom.get(8192, buf, 1));
  EXPECT_FALSE(chip.eeprom.get(8190, buf, 4));
  EXPECT_FALSE(chip.eeprom.put(8192, 0x01));
  EXPECT_TRUE(chip.bus.log.empty());
  EXPECT_TRUE(chip.eeprom.get(8188, buf, 4));
}

// A 4 Kbit part has 512 bytes but one address byte: the second half is refused, not aliased.
TEST(I2CEeprom, RefusesWhatAOneByteAddressCannotReach) {
  Chip chip(512);
  uint8_t buf[2];
  EXPECT_TRUE(chip.eeprom.get(0xFE, buf, 2));
  EXPECT_EQ(chip.bus.log[0].written, (std::vector<uint8_t>{0xFE}));
  EXPECT_FALSE(chip.eeprom.get(0xFF, buf, 2));
  EXPECT_FALSE(chip.eeprom.get(0x145, buf, 1));
  EXPECT_FALSE(chip.eeprom.put(0x100, 0x01));
  EXPECT_EQ(chip.bus.log.size(), 2u);
}

// A write that stays inside one page is one transaction: the address bytes, then the data.
TEST(I2CEeprom, WritesInsideOnePageInOneTransaction) {
  Chip chip(8192);
  const uint8_t data[3] = {0xAA, 0xBB, 0xCC};
  ASSERT_TRUE(chip.eeprom.put(0x0200, data, sizeof(data)));

  ASSERT_EQ(chip.bus.log.size(), 1u);
  EXPECT_EQ(chip.bus.log[0].written, (std::vector<uint8_t>{0x02, 0x00, 0xAA, 0xBB, 0xCC}));
  EXPECT_EQ(chip.bus.log[0].read, 0u);
  EXPECT_EQ(memcmp(chip.bus.image.data() + 0x200, data, sizeof(data)), 0);

  ASSERT_TRUE(chip.eeprom.put(0x0203, 0xDD));
  EXPECT_EQ(chip.bus.log[1].written, (std::vector<uint8_t>{0x02, 0x03, 0xDD}));
  EXPECT_EQ(chip.bus.image[0x203], 0xDD);

  // A whole page from its first byte, the longest write that is still one transaction.
  const std::vector<uint8_t> page = pattern(8);
  ASSERT_TRUE(chip.eeprom.put(0x0208, page.data(), page.size()));
  ASSERT_EQ(chip.bus.log.size(), 3u);
  EXPECT_EQ(chip.bus.log[2].written.size(), 2u + page.size());
  EXPECT_EQ(memcmp(chip.bus.image.data() + 0x208, page.data(), page.size()), 0);
}

// The fake wraps within the page as the hardware does. Without it every split below would pass
// unsplit, so this is what makes the cases that follow mean anything.
TEST(I2CEeprom, TheFakeBusRollsOverInsideThePage) {
  RecordingBus bus;
  const uint8_t frame[6] = {0x02, 0x06, 0xA0, 0xA1, 0xA2, 0xA3};
  ASSERT_EQ(bus.write_readv(0x54, frame, sizeof(frame), nullptr, 0), i2c::ERROR_OK);
  EXPECT_EQ(bus.image[0x206], 0xA0);
  EXPECT_EQ(bus.image[0x207], 0xA1);
  EXPECT_EQ(bus.image[0x200], 0xA2);
  EXPECT_EQ(bus.image[0x201], 0xA3);
  EXPECT_EQ(bus.image[0x208], static_cast<uint8_t>(0x208 * 7));
}

// The regression: as one transaction the last four bytes would land on the first four of the
// page 0x0200 instead of reaching 0x0208, and the write would still report success.
TEST(I2CEeprom, SplitsAWriteThatCrossesAPageBoundary) {
  Chip chip(8192);
  const std::vector<uint8_t> data = pattern(12);
  ASSERT_TRUE(chip.eeprom.put(0x0200, data.data(), data.size()));

  const std::vector<uint8_t> stored(chip.bus.image.begin() + 0x200, chip.bus.image.begin() + 0x200 + data.size());
  EXPECT_EQ(stored, data);
  ASSERT_EQ(chip.bus.log.size(), 2u);
  EXPECT_EQ(chip.bus.log[0].written.size(), 2u + 8u);
  EXPECT_EQ(chip.bus.log[1].written.size(), 2u + 4u);
  EXPECT_EQ(chip.bus.log[1].written[0], 0x02);
  EXPECT_EQ(chip.bus.log[1].written[1], 0x08);
}

// The chunk is the distance to the next boundary, not a fixed count: six bytes from 0x0205 go
// out as three and three. Capping the length at a page alone would still have wrapped them.
TEST(I2CEeprom, SplitsAtTheBoundaryNotAfterAFixedCount) {
  Chip chip(8192);
  const uint8_t data[6] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36};
  ASSERT_TRUE(chip.eeprom.put(0x0205, data, sizeof(data)));

  ASSERT_EQ(chip.bus.log.size(), 2u);
  EXPECT_EQ(chip.bus.log[0].written, (std::vector<uint8_t>{0x02, 0x05, 0x31, 0x32, 0x33}));
  EXPECT_EQ(chip.bus.log[1].written, (std::vector<uint8_t>{0x02, 0x08, 0x34, 0x35, 0x36}));
  EXPECT_EQ(memcmp(chip.bus.image.data() + 0x205, data, sizeof(data)), 0);
}

// Twenty bytes from 0x0106: the rest of that page, two whole ones, then what is left.
TEST(I2CEeprom, SplitsAWriteSpanningSeveralPages) {
  Chip chip(8192);
  const std::vector<uint8_t> data = pattern(20);
  ASSERT_TRUE(chip.eeprom.put(0x0106, data.data(), data.size()));

  ASSERT_EQ(chip.bus.log.size(), 4u);
  EXPECT_EQ(chip.bus.log[0].written, (std::vector<uint8_t>{0x01, 0x06, 0xA0, 0xA1}));
  EXPECT_EQ(chip.bus.log[1].written.size(), 2u + 8u);
  EXPECT_EQ(chip.bus.log[1].written[1], 0x08);
  EXPECT_EQ(chip.bus.log[2].written[1], 0x10);
  EXPECT_EQ(chip.bus.log[3].written, (std::vector<uint8_t>{0x01, 0x18, 0xB2, 0xB3}));
  EXPECT_EQ(memcmp(chip.bus.image.data() + 0x106, data.data(), data.size()), 0);
  // Neither neighbour of the range was touched.
  EXPECT_EQ(chip.bus.image[0x105], static_cast<uint8_t>(0x105 * 7));
  EXPECT_EQ(chip.bus.image[0x11A], static_cast<uint8_t>(0x11A * 7));
}

// Why the default is 8: on a part that really pages 16 and did not say so, an 8-byte-aligned
// split still lands inside a real page, and the bytes arrive where they were meant to.
TEST(I2CEeprom, TheDefaultSplitFitsInsideABiggerRealPage) {
  Chip chip(8192);
  chip.bus.page = 16;
  const std::vector<uint8_t> data = pattern(12);
  ASSERT_TRUE(chip.eeprom.put(0x0204, data.data(), data.size()));

  ASSERT_EQ(chip.bus.log.size(), 2u);
  EXPECT_EQ(memcmp(chip.bus.image.data() + 0x204, data.data(), data.size()), 0);
}

// Naming the part's own page size only saves transactions; the bytes land the same either way.
TEST(I2CEeprom, AnExplicitPageSizeWritesFewerTransactions) {
  const std::vector<uint8_t> data = pattern(20);

  Chip conservative(8192);
  EXPECT_EQ(conservative.eeprom.get_page_size(), 8u);
  ASSERT_TRUE(conservative.eeprom.put(0x0100, data.data(), data.size()));
  EXPECT_EQ(conservative.bus.log.size(), 3u);

  Chip named(8192, 32);
  EXPECT_EQ(named.eeprom.get_page_size(), 32u);
  ASSERT_TRUE(named.eeprom.put(0x0100, data.data(), data.size()));
  EXPECT_EQ(named.bus.log.size(), 1u);
  EXPECT_EQ(memcmp(named.bus.image.data() + 0x100, data.data(), data.size()), 0);
  EXPECT_EQ(memcmp(conservative.bus.image.data() + 0x100, data.data(), data.size()), 0);
}

// No YAML produces a page size of 0, but a lambda can, and the split divides by it.
TEST(I2CEeprom, APageSizeOfZeroWritesOneByteAtATime) {
  Chip chip(8192, 1);
  chip.eeprom.set_page_size(0);
  EXPECT_EQ(chip.eeprom.get_page_size(), 1u);

  const std::vector<uint8_t> data = pattern(3);
  ASSERT_TRUE(chip.eeprom.put(0x0100, data.data(), data.size()));
  EXPECT_EQ(chip.bus.log.size(), 3u);
  EXPECT_EQ(memcmp(chip.bus.image.data() + 0x100, data.data(), data.size()), 0);
}

// A split write is not all-or-nothing: the chunks before the failed one stay written.
TEST(I2CEeprom, AFailedChunkStopsTheWriteAndLeavesTheEarlierOnesWritten) {
  Chip chip(8192);
  chip.bus.refuse_write = 1;
  const std::vector<uint8_t> data = pattern(20);
  EXPECT_FALSE(chip.eeprom.put(0x0200, data.data(), data.size()));

  ASSERT_EQ(chip.bus.log.size(), 2u);
  EXPECT_EQ(memcmp(chip.bus.image.data() + 0x200, data.data(), 8), 0);
  EXPECT_EQ(chip.bus.image[0x208], static_cast<uint8_t>(0x208 * 7));
  EXPECT_EQ(chip.bus.image[0x213], static_cast<uint8_t>(0x213 * 7));
}

TEST(I2CEeprom, WritesAOneBytePartWithAOneByteAddress) {
  Chip chip(256);
  ASSERT_TRUE(chip.eeprom.put(0x7F, 0x42));
  EXPECT_EQ(chip.bus.log[0].written, (std::vector<uint8_t>{0x7F, 0x42}));
  EXPECT_EQ(chip.bus.image[0x7F], 0x42);
}

// What jethome_board_info turns on for the CPU board's EEPROM: nothing reaches the bus.
TEST(I2CEeprom, WriteProtectionRefusesWritesAndLeavesReadsAlone) {
  Chip chip(8192);
  EXPECT_FALSE(chip.eeprom.is_write_protected());
  chip.eeprom.set_write_protected(true);

  const uint8_t data[2] = {0xAA, 0xBB};
  EXPECT_FALSE(chip.eeprom.put(0x0000, data, sizeof(data)));
  EXPECT_FALSE(chip.eeprom.put(0x1FFF, 0x01));
  EXPECT_TRUE(chip.bus.log.empty());
  EXPECT_EQ(chip.bus.image[0x1FFF], static_cast<uint8_t>(0x1FFF * 7));

  uint8_t byte = 0;
  EXPECT_TRUE(chip.eeprom.get(0x0000, &byte));

  chip.eeprom.set_write_protected(false);
  EXPECT_TRUE(chip.eeprom.put(0x0000, 0x42));
  EXPECT_EQ(chip.bus.image[0], 0x42);
}

TEST(I2CEeprom, AChipThatDoesNotAnswerFailsEverything) {
  Chip chip(8192);
  chip.bus.present_at = 0x50;
  uint8_t byte = 0;
  EXPECT_FALSE(chip.eeprom.is_connected());
  EXPECT_FALSE(chip.eeprom.get(0x0000, &byte));
  EXPECT_FALSE(chip.eeprom.put(0x0000, 0x01));
  EXPECT_EQ(byte, 0);
}

TEST(I2CEeprom, ARefusedReadFailsGet) {
  Chip chip(8192);
  chip.bus.refuse_reads = true;
  uint8_t byte = 0;
  EXPECT_TRUE(chip.eeprom.put(0x0000, 0x01));
  EXPECT_FALSE(chip.eeprom.get(0x0000, &byte));
  EXPECT_FALSE(chip.eeprom.is_connected());
}

TEST(I2CEeprom, SetupProbesTheChipAndFiresOnSetup) {
  Chip chip(8192);
  bool fired = false;
  auto *automation = new Automation<>(chip.eeprom.get_setup_trigger());
  automation->add_action(new LambdaAction<>([&fired]() { fired = true; }));

  chip.eeprom.setup();
  EXPECT_FALSE(chip.eeprom.is_failed());
  EXPECT_TRUE(fired);
  ASSERT_EQ(chip.bus.log.size(), 1u);
  EXPECT_EQ(chip.bus.log[0].read, 1u);
}

TEST(I2CEeprom, SetupMarksTheComponentFailedWithoutAChip) {
  Chip chip(8192);
  chip.bus.present_at = 0x50;
  bool fired = false;
  auto *automation = new Automation<>(chip.eeprom.get_setup_trigger());
  automation->add_action(new LambdaAction<>([&fired]() { fired = true; }));

  chip.eeprom.setup();
  EXPECT_TRUE(chip.eeprom.is_failed());
  EXPECT_FALSE(fired);
}

TEST(I2CEeprom, ReportsItsSize) {
  Chip chip(8192);
  EXPECT_EQ(chip.eeprom.get_size(), 8192u);
}

}  // namespace
}  // namespace esphome::i2c_eeprom::testing
