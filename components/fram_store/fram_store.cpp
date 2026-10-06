#include "fram_store.h"
#include <algorithm>
#include <cinttypes>
#include <cstring>
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::fram_store {

static const char *const TAG = "fram_store";

static const uint32_t SUPERBLOCK_MAGIC = 0x4D52464A;  // "JFRM"
static const uint16_t SUPERBLOCK_VERSION = 1;
static const size_t FILL_CHUNK = 64;

struct SuperBlock {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t layout_hash;
  uint32_t size;
  uint32_t format_count;
  uint8_t reserved[10];
  uint16_t crc;
} __attribute__((packed));

static_assert(sizeof(SuperBlock) == SUPERBLOCK_SIZE, "superblock size drifted from the on-device layout");

void FramStore::setup() { this->ensure_ready_(); }

void FramStore::dump_config() {
  this->ensure_ready_();

  ESP_LOGCONFIG(TAG, "FRAM store:");
  ESP_LOGCONFIG(TAG, "  Backend: %s, %" PRIu32 " bytes, store from 0x%04X", this->backend_type(),
                this->backend_ == nullptr ? 0 : this->backend_->size(), this->base_offset_);
  ESP_LOGCONFIG(TAG, "  Layout hash: 0x%08" PRIX32 ", formatted %" PRIu32 " time(s), started in %" PRIu32 " us",
                this->layout_hash_, this->format_count_, this->init_duration_us_);

  if (!this->ready_) {
    ESP_LOGE(TAG, "  Store is not available");
    return;
  }

  for (auto *region : this->regions_) {
    ESP_LOGCONFIG(TAG, "  Region %s at 0x%04X, %u bytes", region->kind(), region->offset(), region->size());
  }
}

void FramStore::ensure_ready_() {
  if (this->checked_)
    return;
  this->checked_ = true;

  uint32_t started = micros();
  this->initialise_();
  this->init_duration_us_ = micros() - started;
}

void FramStore::initialise_() {
  if (this->backend_ == nullptr || !this->backend_->is_ready()) {
    ESP_LOGE(TAG, "No usable memory device");
    this->mark_failed();
    return;
  }

  if (this->expected_size_ != 0 && this->backend_->size() != this->expected_size_) {
    ESP_LOGE(TAG, "Device is %" PRIu32 " bytes, the layout was built for %" PRIu32, this->backend_->size(),
             this->expected_size_);
    this->mark_failed();
    return;
  }

  if (this->base_offset_ + SUPERBLOCK_SIZE > this->backend_->size()) {
    ESP_LOGE(TAG, "No room for the superblock at 0x%04X", this->base_offset_);
    this->mark_failed();
    return;
  }

  this->ready_ = true;

  SuperBlockState state = this->check_superblock_();
  if (state == SUPERBLOCK_UNREADABLE) {
    ESP_LOGE(TAG, "Superblock could not be read, refusing to touch the device");
    this->ready_ = false;
    this->mark_failed();
    return;
  }

  // format() initialises the regions itself, so a freshly wiped device is not scanned twice.
  if (state == SUPERBLOCK_STALE) {
    this->format();
    return;
  }

  for (auto *region : this->regions_) {
    region->init();
  }
}

bool FramStore::in_store_(uint16_t address, size_t len) const {
  const uint32_t size = this->backend_->size();
  return address >= this->base_offset_ && address <= size && len <= size - address;
}

// An i2c error must never read as "blank device": formatting on a transient NACK would wipe live data.
FramStore::SuperBlockState FramStore::check_superblock_() {
  SuperBlock block;
  if (!this->backend_->read(this->base_offset_, reinterpret_cast<uint8_t *>(&block), sizeof(block)))
    return SUPERBLOCK_UNREADABLE;

  if (block.magic != SUPERBLOCK_MAGIC || block.version != SUPERBLOCK_VERSION || block.header_size != sizeof(block))
    return SUPERBLOCK_STALE;
  if (crc16(reinterpret_cast<const uint8_t *>(&block), sizeof(block) - sizeof(block.crc)) != block.crc)
    return SUPERBLOCK_STALE;

  // Kept even when the layout no longer matches: the counter tracks the device, not this firmware's layout.
  this->format_count_ = block.format_count;

  return block.layout_hash == this->layout_hash_ && block.size == this->backend_->size() ? SUPERBLOCK_VALID
                                                                                         : SUPERBLOCK_STALE;
}

bool FramStore::write_superblock_() {
  SuperBlock block{};
  block.magic = SUPERBLOCK_MAGIC;
  block.version = SUPERBLOCK_VERSION;
  block.header_size = sizeof(block);
  block.layout_hash = this->layout_hash_;
  block.size = this->backend_->size();
  block.format_count = this->format_count_;
  block.crc = crc16(reinterpret_cast<const uint8_t *>(&block), sizeof(block) - sizeof(block.crc));

  return this->backend_->write(this->base_offset_, reinterpret_cast<const uint8_t *>(&block), sizeof(block));
}

// Wipes everything from base_offset up, not just region headers: leftover bytes can otherwise pass a
// later CRC check. A half-finished format leaves neither the old contents nor a valid store.
bool FramStore::format() {
  if (!this->ready_)
    return false;

  const size_t span = this->backend_->size() - this->base_offset_;
  ESP_LOGW(TAG, "Formatting %zu bytes from 0x%04X", span, this->base_offset_);

  this->format_count_++;
  if (!this->fill(this->base_offset_, 0, span) || !this->write_superblock_()) {
    ESP_LOGE(TAG, "Formatting failed, store is unusable");
    this->ready_ = false;
    this->mark_failed();
    return false;
  }

  for (auto *region : this->regions_) {
    region->init();
  }
  return true;
}

bool FramStore::is_ready() {
  this->ensure_ready_();
  return this->ready_;
}

bool FramStore::read(uint16_t address, uint8_t *data, size_t len) {
  this->ensure_ready_();
  return this->ready_ && this->in_store_(address, len) && this->backend_->read(address, data, len);
}

bool FramStore::write(uint16_t address, const uint8_t *data, size_t len) {
  this->ensure_ready_();
  return this->ready_ && this->in_store_(address, len) && this->backend_->write(address, data, len);
}

// Checks the whole span first: rejecting only the chunk that runs off the end would leave the ones
// before it already written.
bool FramStore::fill(uint16_t address, uint8_t value, size_t len) {
  this->ensure_ready_();
  if (!this->ready_ || !this->in_store_(address, len))
    return false;

  uint8_t buffer[FILL_CHUNK];
  std::memset(buffer, value, sizeof(buffer));

  while (len != 0) {
    size_t chunk = std::min(len, FILL_CHUNK);
    if (!this->backend_->write(address, buffer, chunk))
      return false;
    address += chunk;
    len -= chunk;
  }

  return true;
}

}  // namespace esphome::fram_store
