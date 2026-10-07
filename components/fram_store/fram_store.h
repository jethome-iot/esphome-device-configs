#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "esphome/core/component.h"
#include "fram_region.h"
#include "nvram_backend.h"

namespace esphome::fram_store {

// Bytes the superblock takes at base_offset; codegen places the first region right after it.
static const uint16_t SUPERBLOCK_SIZE = 32;

struct SuperBlock;

// Owns [base_offset, end) of the device: validates the superblock, reformats that range when the layout
// changed, then hands out regions. Nothing outside the range is ever read or written, so several stores
// share one part and a format of one leaves the others alone.
class FramStore : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

  void set_backend(NvramBackend *backend) { this->backend_ = backend; }
  void set_layout_hash(uint32_t layout_hash) { this->layout_hash_ = layout_hash; }
  void set_expected_size(uint32_t expected_size) { this->expected_size_ = expected_size; }
  void set_base_offset(uint16_t base_offset) { this->base_offset_ = base_offset; }
  // The first byte past the store, padding included; codegen passes where the layout ends.
  void set_end(uint32_t end) { this->end_ = end; }
  void add_region(FramRegion *region) { this->regions_.push_back(region); }

  // Device addresses; a span that leaves [base_offset, end) is refused whole.
  bool read(uint16_t address, uint8_t *data, size_t len);
  bool write(uint16_t address, const uint8_t *data, size_t len);
  bool fill(uint16_t address, uint8_t value, size_t len);

  // Wipes the store's own range and rewrites the superblock; every region comes back empty.
  bool format();

  bool is_ready();
  uint16_t base_offset() const { return this->base_offset_; }
  uint32_t end() const { return this->end_; }
  uint32_t format_count() const { return this->format_count_; }
  // How long the superblock check plus every region's init took; the ring scans dominate it.
  uint32_t init_duration_us() const { return this->init_duration_us_; }
  const char *backend_type() const { return this->backend_ == nullptr ? "none" : this->backend_->type(); }

  // Telling a stale superblock from an unreadable one matters: only the first may reformat.
  enum SuperBlockState : uint8_t {
    SUPERBLOCK_VALID,
    SUPERBLOCK_STALE,
    SUPERBLOCK_UNREADABLE,
  };

  enum StaleReason : uint8_t {
    STALE_NONE,
    STALE_BLANK,
    STALE_VERSION,
    STALE_CRC,
    STALE_SIZE,
    STALE_LAYOUT,
  };

 protected:
  // Runs the superblock check on first use, so a consumer that runs before setup() still gets a live store.
  void ensure_ready_();
  void initialise_();
  bool regions_fit_();
  bool in_store_(uint16_t address, size_t len) const;
  SuperBlockState check_superblock_();
  StaleReason judge_superblock_(const SuperBlock &block) const;
  void log_stale_(const SuperBlock &block, StaleReason reason) const;
  bool write_superblock_();

  NvramBackend *backend_{nullptr};
  std::vector<FramRegion *> regions_;
  uint32_t layout_hash_{0};
  uint32_t expected_size_{0};
  uint32_t end_{0};
  uint32_t format_count_{0};
  uint32_t init_duration_us_{0};
  uint16_t base_offset_{0};
  bool ready_{false};
  bool checked_{false};
};

}  // namespace esphome::fram_store
