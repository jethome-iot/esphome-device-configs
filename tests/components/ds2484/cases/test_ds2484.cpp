// The status wait over a fake DS2484 whose busy bit stays set for a given time after a command:
// a wait outlasts a 1-Wire reset however fast the bus reads, and still gives up on a chip that
// never comes back.

#include <gtest/gtest.h>
#include <vector>
#include "esphome/components/ds2484/ds2484.h"
#include "esphome/core/hal.h"

namespace esphome::ds2484::testing {
namespace {

// Reads answer the status register: busy (bit 0) until busy_us after the last command.
class FakeDS2484 : public i2c::I2CBus {
 public:
  uint32_t busy_us{0};
  bool busy_forever{false};
  bool refuse_reads{false};
  // The status read with this index (0-based) returns only after this many microseconds, as a
  // read the loop task is preempted around does.
  int stall_read{-1};
  uint32_t stall_us{0};
  size_t status_reads{0};
  std::vector<uint8_t> commands;

  i2c::ErrorCode write_readv(uint8_t addr, const uint8_t *write_buffer, size_t write_count, uint8_t *read_buffer,
                             size_t read_count) override {
    if (addr != 0x18)
      return i2c::ERROR_NOT_ACKNOWLEDGED;
    if (write_count != 0) {
      this->commands.push_back(write_buffer[0]);
      this->since_ = micros();
    }
    if (read_count != 0) {
      if (this->refuse_reads)
        return i2c::ERROR_TIMEOUT;
      const bool busy = this->busy_forever || micros() - this->since_ < this->busy_us;
      read_buffer[0] = busy ? 0x01 : 0x00;
      if (static_cast<int>(this->status_reads) == this->stall_read)
        delayMicroseconds(this->stall_us);
      this->status_reads++;
    }
    return i2c::ERROR_OK;
  }

 private:
  uint32_t since_{0};
};

struct Bus {
  FakeDS2484 chip;
  DS2484OneWireBus ds;

  Bus() {
    this->ds.set_i2c_bus(&this->chip);
    this->ds.set_i2c_address(0x18);
  }
};

TEST(DS2484, AResetLongerThanTenReadsStillCompletes) {
  Bus bus;
  bus.chip.busy_us = 3000;
  EXPECT_EQ(bus.ds.reset_int(), 1);
  EXPECT_EQ(bus.chip.commands, std::vector<uint8_t>({0xb4}));
  EXPECT_GT(bus.chip.status_reads, 10u) << "the fake's reads are faster than a real bus";
}

TEST(DS2484, AChipReadyAtOnceTakesOneRead) {
  Bus bus;
  EXPECT_EQ(bus.ds.reset_int(), 1);
  EXPECT_EQ(bus.chip.status_reads, 1u);
}

TEST(DS2484, AChipThatStaysBusyIsGivenUpAfterTheTimeout) {
  Bus bus;
  bus.chip.busy_forever = true;
  const uint32_t start = micros();
  EXPECT_EQ(bus.ds.reset_int(), 0);
  const uint32_t took = micros() - start;
  EXPECT_GE(took, 5000u);
  EXPECT_LT(took, 100000u) << "the wait is bounded";
}

// Busy when read, then held past the timeout before the next read: the chip finished meanwhile, and
// one more read says so instead of the timeout giving up on it.
TEST(DS2484, AWaitPreemptedPastTheTimeoutReadsOnceMore) {
  Bus bus;
  bus.chip.busy_us = 2000;
  bus.chip.stall_read = 0;
  bus.chip.stall_us = 7000;
  EXPECT_EQ(bus.ds.reset_int(), 1);
  EXPECT_EQ(bus.chip.status_reads, 2u);
}

TEST(DS2484, AReadErrorEndsTheWaitAtOnce) {
  Bus bus;
  bus.chip.busy_forever = true;
  bus.chip.refuse_reads = true;
  EXPECT_EQ(bus.ds.reset_int(), 0);
  EXPECT_EQ(bus.chip.status_reads, 0u);
}

TEST(DS2484, AFailedCommandWriteIsNoReset) {
  Bus bus;
  bus.ds.set_i2c_address(0x19);
  EXPECT_EQ(bus.ds.reset_int(), -1);
  EXPECT_TRUE(bus.chip.commands.empty());
}

}  // namespace
}  // namespace esphome::ds2484::testing
