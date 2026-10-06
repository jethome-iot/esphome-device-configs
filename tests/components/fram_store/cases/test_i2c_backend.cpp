// The store over a real i2c_eeprom component in FRAM mode, on a fake bus that holds the part's
// bytes: what the firmware runs, minus the wire.

#include "common.h"
#include "esphome/components/fram_store/i2c_eeprom_backend.h"
#include "esphome/components/i2c_eeprom/i2c_eeprom.h"

namespace esphome::fram_store::testing {
namespace {

static const uint8_t FRAM_ADDRESS = 0x52;
static const uint32_t FRAM_SIZE = 8192;
static const uint16_t STORE_BASE = 0x200;
static const uint16_t ODOMETERS = 200;

// An FM24CL64B: a two-byte address, then a write streams into the array with no page to wrap at.
class FakeFramBus : public i2c::I2CBus {
 public:
  std::vector<uint8_t> image = std::vector<uint8_t>(FRAM_SIZE, 0);
  bool present{true};
  bool refuse_reads{false};
  size_t longest_transfer{0};
  uint32_t lowest_write{FRAM_SIZE};

  i2c::ErrorCode write_readv(uint8_t addr, const uint8_t *write_buffer, size_t write_count, uint8_t *read_buffer,
                             size_t read_count) override {
    if (!this->present || addr != FRAM_ADDRESS)
      return i2c::ERROR_NOT_ACKNOWLEDGED;
    this->longest_transfer = std::max({this->longest_transfer, write_count, read_count});
    if (write_count >= 2) {
      this->pointer_ = (static_cast<size_t>(write_buffer[0]) << 8) | write_buffer[1];
      if (write_count > 2)
        this->lowest_write = std::min<uint32_t>(this->lowest_write, this->pointer_);
      for (size_t i = 2; i < write_count; i++)
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

// The part as fram.yaml declares it, found by its setup() the way the firmware finds it.
struct Part {
  FakeFramBus bus;
  i2c_eeprom::I2CEeprom fram;
  I2CEepromBackend backend{&fram};

  explicit Part(uint32_t size = FRAM_SIZE) {
    this->fram.set_i2c_bus(&this->bus);
    this->fram.set_i2c_address(FRAM_ADDRESS);
    this->fram.set_size(size);
    this->fram.set_fram(true);
  }
  void boot() { this->fram.setup(); }
};

// One boot's store: an odometer-sized slot and a ring, from 0x200 up.
struct Boot {
  FramStore store;
  FramSlot slot;
  FramRing ring;

  explicit Boot(NvramBackend *backend)
      : slot(&store, STORE_BASE + SUPERBLOCK_SIZE, ODOMETERS),
        ring(&store, STORE_BASE + SUPERBLOCK_SIZE + 2 * (FramSlot::HEADER_SIZE + ODOMETERS), RING_RECORD, 16) {
    store.set_backend(backend);
    store.set_expected_size(FRAM_SIZE);
    store.set_base_offset(STORE_BASE);
    store.set_layout_hash(LAYOUT_HASH);
    store.add_region(&slot);
    store.add_region(&ring);
    store.setup();
  }
};

std::vector<uint8_t> payload(uint8_t seed) {
  std::vector<uint8_t> data(ODOMETERS);
  for (size_t i = 0; i < data.size(); i++)
    data[i] = static_cast<uint8_t>(seed + i);
  return data;
}

class I2CBackendTest : public ::testing::Test {
 protected:
  void SetUp() override { LogCapture::instance().clear(); }
};

// The store reads its superblock in setup(), so it has to come after the part has found its chip.
TEST_F(I2CBackendTest, SetsUpAfterThePart) {
  Part part;
  FramStore store;
  EXPECT_LT(store.get_setup_priority(), part.fram.get_setup_priority());
}

TEST_F(I2CBackendTest, DescribesThePart) {
  Part part;
  EXPECT_STREQ(part.backend.type(), "FRAM");
  EXPECT_EQ(part.backend.size(), FRAM_SIZE);
  part.fram.set_fram(false);
  EXPECT_STREQ(part.backend.type(), "EEPROM");
}

TEST_F(I2CBackendTest, AValueSurvivesARestart) {
  Part part;
  part.boot();
  ASSERT_TRUE(part.backend.is_ready());
  {
    Boot first(&part.backend);
    ASSERT_TRUE(first.store.is_ready());
    EXPECT_STREQ(first.store.backend_type(), "FRAM");
    const std::vector<uint8_t> data = payload(7);
    ASSERT_TRUE(first.slot.save(data.data(), data.size()));
    uint64_t record = 99;
    ASSERT_TRUE(first.ring.append(record));
  }

  Boot second(&part.backend);
  EXPECT_EQ(second.store.format_count(), 1u);
  std::vector<uint8_t> read(ODOMETERS);
  ASSERT_TRUE(second.slot.load(read.data(), read.size()));
  EXPECT_EQ(read, payload(7));
  uint64_t record = 0;
  ASSERT_TRUE(second.ring.peek(0, record));
  EXPECT_EQ(record, 99u);
}

// The format, the scans and an odometer-sized save all go out in bounded transfers, above the base.
TEST_F(I2CBackendTest, StaysAboveTheBaseInBoundedTransfers) {
  Part part;
  std::fill(part.bus.image.begin(), part.bus.image.begin() + STORE_BASE, 0xA5);
  part.boot();
  Boot boot(&part.backend);
  const std::vector<uint8_t> data = payload(1);
  ASSERT_TRUE(boot.slot.save(data.data(), data.size()));

  EXPECT_LE(part.bus.longest_transfer, 2u + i2c_eeprom::I2CEeprom::MAX_TRANSFER);
  EXPECT_EQ(part.bus.lowest_write, STORE_BASE);
  EXPECT_TRUE(std::all_of(part.bus.image.begin(), part.bus.image.begin() + STORE_BASE,
                          [](uint8_t byte) { return byte == 0xA5; }));
}

TEST_F(I2CBackendTest, AMissingPartLeavesTheStoreUnavailable) {
  Part part;
  part.bus.present = false;
  part.boot();
  EXPECT_FALSE(part.backend.is_ready());

  Boot boot(&part.backend);
  EXPECT_FALSE(boot.store.is_ready());
  EXPECT_TRUE(boot.store.is_failed());
  EXPECT_TRUE(LogCapture::instance().has_error("No usable memory device"));
}

// A part that stops answering after a good boot must not be taken for a blank one.
TEST_F(I2CBackendTest, APartThatStopsAnsweringIsNotFormatted) {
  Part part;
  part.boot();
  {
    Boot first(&part.backend);
    const std::vector<uint8_t> data = payload(3);
    ASSERT_TRUE(first.slot.save(data.data(), data.size()));
  }
  const std::vector<uint8_t> before = part.bus.image;

  part.bus.refuse_reads = true;
  Boot silent(&part.backend);
  EXPECT_FALSE(silent.store.is_ready());
  EXPECT_TRUE(LogCapture::instance().has_error("Superblock could not be read"));
  EXPECT_EQ(part.bus.image, before);
}

TEST_F(I2CBackendTest, APartOfAnotherSizeIsRefused) {
  Part part(4096);
  part.boot();
  Boot boot(&part.backend);
  EXPECT_FALSE(boot.store.is_ready());
  EXPECT_TRUE(LogCapture::instance().has_error("Device is 4096 bytes, the layout was built for 8192"));
}

}  // namespace
}  // namespace esphome::fram_store::testing
