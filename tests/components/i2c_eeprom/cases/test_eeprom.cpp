// The component over a bus that records every transaction: how a memory address is framed for
// each part size, what a write carries, and what a chip that does not answer does to it.

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
// longer write stores the bytes that follow the address.
class RecordingBus : public i2c::I2CBus {
 public:
  std::vector<Transaction> log;
  std::vector<uint8_t> image;
  uint8_t present_at{0x54};
  size_t address_bytes{2};
  bool refuse_reads{false};

  RecordingBus() : image(8192) {
    for (size_t i = 0; i < this->image.size(); i++)
      this->image[i] = static_cast<uint8_t>(i * 7);
  }

  i2c::ErrorCode write_readv(uint8_t addr, const uint8_t *write_buffer, size_t write_count, uint8_t *read_buffer,
                             size_t read_count) override {
    this->log.push_back({addr, std::vector<uint8_t>(write_buffer, write_buffer + write_count), read_count});
    if (addr != this->present_at)
      return i2c::ERROR_NOT_ACKNOWLEDGED;
    if (write_count >= this->address_bytes) {
      this->pointer_ = 0;
      for (size_t i = 0; i < this->address_bytes; i++)
        this->pointer_ = (this->pointer_ << 8) | write_buffer[i];
      for (size_t i = this->address_bytes; i < write_count; i++)
        this->image.at(this->pointer_++) = write_buffer[i];
    }
    if (read_count != 0 && this->refuse_reads)
      return i2c::ERROR_TIMEOUT;
    for (size_t i = 0; i < read_count; i++)
      read_buffer[i] = this->image.at(this->pointer_++);
    return i2c::ERROR_OK;
  }

 protected:
  size_t pointer_{0};
};

struct Chip {
  RecordingBus bus;
  I2CEeprom eeprom;

  explicit Chip(uint32_t size_bytes) {
    this->eeprom.set_i2c_bus(&this->bus);
    this->eeprom.set_i2c_address(0x54);
    this->eeprom.set_size(size_bytes);
    this->bus.address_bytes = size_bytes > 2048 ? 2 : 1;
  }
};

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

TEST(I2CEeprom, WritesTheAddressAndTheDataInOneTransaction) {
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
}

TEST(I2CEeprom, WritesAOneBytePartWithAOneByteAddress) {
  Chip chip(256);
  ASSERT_TRUE(chip.eeprom.put(0x7F, 0x42));
  EXPECT_EQ(chip.bus.log[0].written, (std::vector<uint8_t>{0x7F, 0x42}));
  EXPECT_EQ(chip.bus.image[0x7F], 0x42);
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
