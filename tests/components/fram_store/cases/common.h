#pragma once

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/fram_store/fram_region.h"
#include "esphome/components/fram_store/fram_store.h"
#include "esphome/components/fram_store/nvram_backend.h"
#include "esphome/components/logger/logger.h"

namespace esphome::fram_store::testing {

static const uint32_t DEVICE_SIZE = 512;
static const uint32_t LAYOUT_HASH = 0xABCD1234;
static const uint16_t SLOT_OFFSET = SUPERBLOCK_SIZE;
static const uint16_t SLOT_PAYLOAD = 8;
static const uint16_t RING_RECORD = 8;
static const uint16_t RING_CAPACITY = 4;
static const uint16_t RING_OFFSET = SLOT_OFFSET + 2 * (FramSlot::HEADER_SIZE + SLOT_PAYLOAD);
// Where the shared layout ends, as codegen would pass it; the device goes on past it.
static const uint32_t STORE_END = RING_OFFSET + RING_CAPACITY * (FramRing::HEADER_SIZE + RING_RECORD);

// Copy 1 of the slot, and the payload inside a copy or a record: the addresses the tests damage.
inline uint16_t slot_copy(uint8_t copy) { return SLOT_OFFSET + copy * (FramSlot::HEADER_SIZE + SLOT_PAYLOAD); }
inline uint16_t slot_payload(uint8_t copy) { return slot_copy(copy) + FramSlot::HEADER_SIZE; }
inline uint16_t ring_entry(uint16_t index) { return RING_OFFSET + index * (FramRing::HEADER_SIZE + RING_RECORD); }
inline uint16_t ring_payload(uint16_t index) { return ring_entry(index) + FramRing::HEADER_SIZE; }

// A backend that can be told to fail, which is the only way to reach the "device did not answer" paths:
// they are the ones that must never be confused with "device is blank".
class FaultyBackend : public RamBackend {
 public:
  using RamBackend::RamBackend;

  // The next `count` reads covering `address` fail; by default until stop_failing().
  void fail_reads_covering(uint16_t address, uint32_t count = UINT32_MAX) {
    this->fail_address_ = address;
    this->failing_ = count;
  }
  void stop_failing() { this->failing_ = 0; }
  // The next `count` reads covering `address` succeed with a wrong byte there: a different one each time,
  // or the same one when `same`, which is what a part returning garbage that looks stable does.
  void garble_reads_covering(uint16_t address, uint32_t count, bool same = false) {
    this->garble_address_ = address;
    this->garbling_ = count;
    this->garble_same_ = same;
  }
  void fail_writes(bool fail) { this->failing_writes_ = fail; }
  // Every write below this address fails the test: it is someone else's memory.
  void guard_below(uint16_t address) { this->guard_ = address; }

  bool write(uint16_t address, const uint8_t *data, size_t len) override {
    EXPECT_GE(address, this->guard_) << "wrote " << len << " bytes below the store";
    return this->failing_writes_ ? false : RamBackend::write(address, data, len);
  }

  bool read(uint16_t address, uint8_t *data, size_t len) override {
    EXPECT_GE(address, this->guard_) << "read " << len << " bytes below the store";
    this->reads_++;
    if (this->failing_ != 0 && covers(address, len, this->fail_address_)) {
      if (this->failing_ != UINT32_MAX)
        this->failing_--;
      return false;
    }
    if (!RamBackend::read(address, data, len))
      return false;
    if (this->garbling_ != 0 && covers(address, len, this->garble_address_)) {
      this->garbling_--;
      this->garbled_++;
      data[this->garble_address_ - address] ^= this->garble_same_ ? 0xFF : this->garbled_;
    }
    return true;
  }

  uint32_t reads() const { return this->reads_; }
  void reset_reads() { this->reads_ = 0; }

 protected:
  static bool covers(uint16_t address, size_t len, uint16_t target) {
    return address <= target && target < address + len;
  }

  uint16_t fail_address_{0};
  uint16_t garble_address_{0};
  uint16_t guard_{0};
  uint32_t reads_{0};
  uint32_t failing_{0};
  uint32_t garbling_{0};
  uint8_t garbled_{0};
  bool garble_same_{false};
  bool failing_writes_{false};
};

// Every error and warning logged since clear(). Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() {
    this->errors.clear();
    this->warnings.clear();
  }
  bool has_error(const char *needle) const { return has(this->errors, needle); }
  bool has_warning(const char *needle) const { return has(this->warnings, needle); }

 protected:
  static bool has(const std::vector<std::string> &lines, const char *needle) {
    return std::any_of(lines.begin(), lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    auto *capture = static_cast<LogCapture *>(self);
    if (level == ESPHOME_LOG_LEVEL_ERROR) {
      capture->errors.emplace_back(message, len);
    } else if (level == ESPHOME_LOG_LEVEL_WARN) {
      capture->warnings.emplace_back(message, len);
    }
  }
};

// One boot of the shared layout: the superblock at 0, a slot, then a ring.
struct Mounted {
  FramStore store;
  FramSlot slot;
  FramRing ring;

  explicit Mounted(NvramBackend *backend, uint32_t layout_hash = LAYOUT_HASH)
      : slot(&store, SLOT_OFFSET, SLOT_PAYLOAD), ring(&store, RING_OFFSET, RING_RECORD, RING_CAPACITY) {
    store.set_backend(backend);
    store.set_expected_size(DEVICE_SIZE);
    store.set_end(STORE_END);
    store.set_layout_hash(layout_hash);
    store.add_region(&slot);
    store.add_region(&ring);
    store.setup();
  }
};

// One device, rebuilt stores: a fresh store over the same backend is what a reboot looks like from the
// format's point of view, which is the only way to test recovery without hardware.
class FramStoreTest : public ::testing::Test {
 protected:
  void SetUp() override { LogCapture::instance().clear(); }

  std::unique_ptr<Mounted> mount(uint32_t layout_hash = LAYOUT_HASH) {
    return std::make_unique<Mounted>(&this->backend_, layout_hash);
  }

  // Overwrites a payload without touching its header: the shape a torn write leaves behind.
  void corrupt(uint16_t address) {
    uint8_t garbage[SLOT_PAYLOAD];
    std::memset(garbage, 0x5A, sizeof(garbage));
    ASSERT_TRUE(this->backend_.write(address, garbage, sizeof(garbage)));
  }

  FaultyBackend backend_{DEVICE_SIZE};
};

// Geometries the shared fixture cannot express: a one-record ring, and one long enough that the header
// scan reads several batches.
class FramRingGeometryTest : public ::testing::Test {
 protected:
  struct Rig {
    RamBackend own_backend;
    FramStore store;
    FramRing ring;

    Rig(uint32_t device_size, uint16_t record_size, uint16_t capacity, RamBackend *shared = nullptr)  // NOLINT
        : own_backend(device_size), ring(&store, SLOT_OFFSET, record_size, capacity) {
      this->device = shared == nullptr ? &this->own_backend : shared;
      store.set_backend(this->device);
      store.set_expected_size(device_size);
      store.set_end(SLOT_OFFSET + ring.size());
      store.set_layout_hash(LAYOUT_HASH);
      store.add_region(&ring);
      store.setup();
    }

    RamBackend *device{nullptr};
  };

  // A fresh store over the same device: what the ring sees after a reboot.
  std::unique_ptr<Rig> remount(Rig &previous, uint16_t record_size, uint16_t capacity) {
    return std::make_unique<Rig>(previous.device->size(), record_size, capacity, previous.device);
  }

  void damage_record(Rig &rig, uint16_t index) {
    uint8_t garbage[RING_RECORD];
    std::memset(garbage, 0x5A, sizeof(garbage));
    uint16_t payload = SLOT_OFFSET + index * (FramRing::HEADER_SIZE + RING_RECORD) + FramRing::HEADER_SIZE;
    ASSERT_TRUE(rig.device->write(payload, garbage, sizeof(garbage)));
  }
};

}  // namespace esphome::fram_store::testing
