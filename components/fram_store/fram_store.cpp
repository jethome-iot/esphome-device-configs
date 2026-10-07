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
static const size_t FILL_CHUNK = 128;

// A NACK on the shared bus is worth a retry, and a stale verdict has to hold for several reads.
static const uint8_t SUPERBLOCK_ATTEMPTS = 4;
static const uint8_t STALE_READS = 3;
static const uint32_t RETRY_PAUSE_MS = 5;

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
  ESP_LOGCONFIG(TAG, "  Backend: %s, %" PRIu32 " bytes, store 0x%04X-0x%04" PRIX32, this->backend_type(),
                this->backend_ == nullptr ? 0 : this->backend_->size(), this->base_offset_,
                this->end_ == 0 ? 0 : this->end_ - 1);
  ESP_LOGCONFIG(TAG, "  Layout hash: 0x%08" PRIX32 ", formatted %" PRIu32 " time(s), started in %" PRIu32 " us",
                this->layout_hash_, this->format_count_, this->init_duration_us_);

  if (!this->ready_) {
    ESP_LOGE(TAG, "  Store is not available");
    return;
  }

  for (auto *region : this->regions_) {
    ESP_LOGCONFIG(TAG, "  Region %s at 0x%04X, %" PRIu32 " bytes", region->kind(), region->offset(), region->size());
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

  const uint32_t size = this->backend_->size();
  if (this->expected_size_ != 0 && size != this->expected_size_) {
    ESP_LOGE(TAG, "Device is %" PRIu32 " bytes, the layout was built for %" PRIu32, size, this->expected_size_);
    this->mark_failed();
    return;
  }

  if (this->end_ > size) {
    ESP_LOGE(TAG, "Store at 0x%04X runs to 0x%04" PRIX32 ", past the %" PRIu32 "-byte device", this->base_offset_,
             this->end_ - 1, size);
    this->mark_failed();
    return;
  }

  if (this->end_ < static_cast<uint32_t>(this->base_offset_) + SUPERBLOCK_SIZE) {
    ESP_LOGE(TAG, "No room for the superblock at 0x%04X", this->base_offset_);
    this->mark_failed();
    return;
  }

  if (!this->regions_fit_()) {
    this->mark_failed();
    return;
  }

  this->ready_ = true;

  SuperBlockState state = this->check_superblock_();
  if (state == SUPERBLOCK_UNREADABLE) {
    this->ready_ = false;
    this->mark_failed();
    return;
  }

  // format() initialises the regions itself, so a freshly wiped store is not scanned twice.
  if (state == SUPERBLOCK_STALE) {
    this->format();
    return;
  }

  for (auto *region : this->regions_) {
    region->init();
  }
}

// Codegen places every region between the superblock and the end; a hand-built store gets the same check.
bool FramStore::regions_fit_() {
  const uint32_t first = static_cast<uint32_t>(this->base_offset_) + SUPERBLOCK_SIZE;
  for (auto *region : this->regions_) {
    const uint32_t start = region->offset();
    if (start < first || start + region->size() > this->end_) {
      ESP_LOGE(TAG, "Region at 0x%04" PRIX32 " (%" PRIu32 " bytes) is outside the store's 0x%04" PRIX32 "-0x%04" PRIX32,
               start, region->size(), first, this->end_ - 1);
      return false;
    }
  }
  return true;
}

bool FramStore::in_store_(uint16_t address, size_t len) const {
  return address >= this->base_offset_ && address <= this->end_ && len <= this->end_ - address;
}

// An i2c error must never read as "blank device", and one garbled read must not wipe a live store: a
// stale verdict stands only once STALE_READS answered reads return the same bytes.
FramStore::SuperBlockState FramStore::check_superblock_() {
  SuperBlock first{};
  uint8_t agreeing = 0;
  bool answered = false;

  for (uint8_t attempt = 0; attempt < SUPERBLOCK_ATTEMPTS; attempt++) {
    if (attempt != 0)
      delay(RETRY_PAUSE_MS);

    SuperBlock block;
    if (!this->backend_->read(this->base_offset_, reinterpret_cast<uint8_t *>(&block), sizeof(block)))
      continue;
    answered = true;

    StaleReason reason = this->judge_superblock_(block);
    if (reason == STALE_NONE) {
      this->format_count_ = block.format_count;
      return SUPERBLOCK_VALID;
    }

    if (agreeing != 0 && std::memcmp(&block, &first, sizeof(block)) == 0) {
      agreeing++;
    } else {
      first = block;
      agreeing = 1;
    }

    if (agreeing == STALE_READS) {
      // Past the CRC the counter is trusted: it tracks the device, not this firmware's layout.
      if (reason == STALE_SIZE || reason == STALE_LAYOUT)
        this->format_count_ = block.format_count;
      this->log_stale_(block, reason);
      return SUPERBLOCK_STALE;
    }
  }

  if (answered) {
    ESP_LOGE(TAG, "Superblock at 0x%04X reads differently each time, refusing to touch the store", this->base_offset_);
  } else {
    ESP_LOGE(TAG, "Superblock at 0x%04X could not be read, refusing to touch the store", this->base_offset_);
  }
  return SUPERBLOCK_UNREADABLE;
}

FramStore::StaleReason FramStore::judge_superblock_(const SuperBlock &block) const {
  if (block.magic != SUPERBLOCK_MAGIC)
    return STALE_BLANK;
  if (block.version != SUPERBLOCK_VERSION || block.header_size != sizeof(block))
    return STALE_VERSION;
  if (crc16(reinterpret_cast<const uint8_t *>(&block), sizeof(block) - sizeof(block.crc)) != block.crc)
    return STALE_CRC;
  if (block.size != this->backend_->size())
    return STALE_SIZE;
  if (block.layout_hash != this->layout_hash_)
    return STALE_LAYOUT;
  return STALE_NONE;
}

void FramStore::log_stale_(const SuperBlock &block, StaleReason reason) const {
  switch (reason) {
    case STALE_BLANK:
      ESP_LOGW(TAG, "No superblock at 0x%04X: the range is blank or holds something else", this->base_offset_);
      break;
    case STALE_VERSION:
      ESP_LOGW(TAG, "Superblock at 0x%04X is version %u of %u bytes, this firmware reads version %u of %u",
               this->base_offset_, block.version, block.header_size, SUPERBLOCK_VERSION, SUPERBLOCK_SIZE);
      break;
    case STALE_CRC:
      ESP_LOGW(TAG, "Superblock at 0x%04X fails its CRC", this->base_offset_);
      break;
    case STALE_SIZE:
      ESP_LOGW(TAG, "Superblock at 0x%04X was written for a %" PRIu32 "-byte device, this one has %" PRIu32,
               this->base_offset_, static_cast<uint32_t>(block.size), this->backend_->size());
      break;
    default:
      ESP_LOGW(TAG, "Layout at 0x%04X changed: hash 0x%08" PRIX32 " -> 0x%08" PRIX32, this->base_offset_,
               static_cast<uint32_t>(block.layout_hash), this->layout_hash_);
      break;
  }
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

// Wipes the whole range, not just region headers: leftover bytes can otherwise pass a later CRC check. The
// superblock is wiped first and written last, so a format cut short leaves no valid store behind.
bool FramStore::format() {
  if (!this->ready_)
    return false;

  const size_t span = this->end_ - this->base_offset_;
  ESP_LOGW(TAG, "Formatting %zu bytes at 0x%04X-0x%04" PRIX32, span, this->base_offset_, this->end_ - 1);

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
