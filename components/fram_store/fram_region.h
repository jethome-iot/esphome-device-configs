#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome::fram_store {

class FramStore;

// A copy or record can be good, damaged, or unreadable -- and only the first two say anything about
// what is on the device, so they must not be collapsed into one "false".
enum class CheckResult : uint8_t {
  OK,
  DAMAGED,
  IO_ERROR,
};

struct SlotHeader {
  uint32_t seq;
  uint16_t len;
  uint16_t crc;
} __attribute__((packed));

struct RingHeader {
  uint32_t seq;
  uint16_t crc;
} __attribute__((packed));

// A fixed span of the device, sized and placed by codegen.
class FramRegion {
 public:
  FramRegion(FramStore *store, uint16_t offset, uint32_t size) : store_(store), offset_(offset), size_(size) {}
  virtual ~FramRegion() = default;

  // Called by the store once the superblock is known good; a region may only touch the device from here on.
  virtual void init() = 0;
  virtual const char *kind() const = 0;

  uint16_t offset() const { return this->offset_; }
  uint32_t size() const { return this->size_; }
  // False when init() could not read the region: its contents are unknown, so it refuses to write over them.
  bool is_initialised() const { return this->initialised_; }

 protected:
  // Offsets from the region's start; a span that runs past the region is refused whole.
  bool read_(uint32_t offset, uint8_t *data, size_t len);
  bool write_(uint32_t offset, const uint8_t *data, size_t len);
  bool fill_(uint32_t offset, uint8_t value, size_t len);
  bool in_region_(uint32_t offset, size_t len) const;
  // Zeroes the region without the is_initialised() guard clear() applies -- only init() may do that.
  bool wipe_();
  // Streams the range through crc16 in small chunks, so a CRC never needs a buffer the size of the record.
  bool crc_of_range_(uint32_t offset, size_t len, uint16_t &crc);

  FramStore *store_;
  uint16_t offset_;
  uint32_t size_;
  bool initialised_{false};
};

// One value in two copies. A write commits by rewriting the header last, so power loss loses the update, not the data.
class FramSlot : public FramRegion {
 public:
  static const uint16_t HEADER_SIZE = 8;

  FramSlot(FramStore *store, uint16_t offset, uint16_t payload_size)
      : FramRegion(store, offset, 2 * (static_cast<uint32_t>(HEADER_SIZE) + payload_size)),
        payload_size_(payload_size) {}

  void init() override;
  const char *kind() const override { return "slot"; }

  // A load that fails on a slot holding data blocks save() until a load succeeds or clear() empties the
  // slot: a consumer that starts over from zero must not overwrite both copies of what it could not read.
  bool load(uint8_t *data, size_t len);
  bool save(const uint8_t *data, size_t len);
  bool clear();

  template<typename T> bool load(T &value) {
    static_assert(std::is_trivially_copyable<T>::value, "FramSlot only stores trivially copyable types");
    return this->load(reinterpret_cast<uint8_t *>(&value), sizeof(T));
  }

  template<typename T> bool save(const T &value) {
    static_assert(std::is_trivially_copyable<T>::value, "FramSlot only stores trivially copyable types");
    return this->save(reinterpret_cast<const uint8_t *>(&value), sizeof(T));
  }

  bool has_data() const { return this->has_data_; }
  // True from a failed load() until a load succeeds or clear() runs; save() refuses meanwhile.
  bool load_failed() const { return this->load_failed_; }
  uint32_t seq() const { return this->seq_; }
  uint16_t payload_size() const { return this->payload_size_; }

 protected:
  uint32_t copy_offset_(uint8_t copy) const {
    return copy * (static_cast<uint32_t>(HEADER_SIZE) + this->payload_size_);
  }
  bool read_header_(uint8_t copy, SlotHeader &header);
  CheckResult validate_copy_(uint8_t copy, const SlotHeader &header);
  CheckResult read_copy_(uint8_t copy, uint8_t *data, uint32_t &seq);

  uint16_t payload_size_;
  uint32_t seq_{0};
  // Highest sequence seen on the device, valid copy or not: a save must never reuse a number already there.
  uint32_t highest_seq_{0};
  uint8_t active_{0};
  bool has_data_{false};
  bool load_failed_{false};
};

// Append-only ring of fixed-size records, each carrying a monotonic sequence number and a CRC.
class FramRing : public FramRegion {
 public:
  static const uint16_t HEADER_SIZE = 6;

  FramRing(FramStore *store, uint16_t offset, uint16_t record_size, uint16_t capacity)
      : FramRegion(store, offset, (static_cast<uint32_t>(HEADER_SIZE) + record_size) * capacity),
        record_size_(record_size),
        capacity_(capacity) {}

  void init() override;
  const char *kind() const override { return "ring"; }

  bool append(const uint8_t *data, size_t len);
  // index 0 is the newest record, 1 the one before it, and so on.
  bool peek(uint16_t index, uint8_t *data, size_t len);
  // Oldest first: a clear cut short leaves the newest records, never a gap among them.
  bool clear();

  template<typename T> bool append(const T &value) {
    static_assert(std::is_trivially_copyable<T>::value, "FramRing only stores trivially copyable types");
    return this->append(reinterpret_cast<const uint8_t *>(&value), sizeof(T));
  }

  template<typename T> bool peek(uint16_t index, T &value) {
    static_assert(std::is_trivially_copyable<T>::value, "FramRing only stores trivially copyable types");
    return this->peek(index, reinterpret_cast<uint8_t *>(&value), sizeof(T));
  }

  uint16_t count() const { return this->count_; }
  uint16_t capacity() const { return this->capacity_; }
  uint16_t record_size() const { return this->record_size_; }
  // Total records ever appended since the last format -- keeps counting past the ring wrapping.
  uint32_t total_appended() const { return this->head_seq_; }
  // True when the binary search could not be trusted and init() fell back to reading every record.
  bool used_sweep() const { return this->used_sweep_; }

 protected:
  uint16_t entry_size_() const { return HEADER_SIZE + this->record_size_; }
  uint32_t entry_offset_(uint16_t index) const { return static_cast<uint32_t>(index) * this->entry_size_(); }
  CheckResult validate_entry_(uint16_t index, uint32_t &seq);
  bool read_seq_(uint16_t index, uint32_t &seq);
  CheckResult locate_head_(uint16_t &index, uint32_t &seq, uint16_t &count);
  bool head_is_consistent_(uint16_t index, uint32_t seq);
  bool scan_(uint16_t *indices, uint32_t *seqs, uint8_t candidates, uint16_t &count);

  uint16_t record_size_;
  uint16_t capacity_;
  uint16_t head_index_{0};
  uint16_t count_{0};
  uint32_t head_seq_{0};
  bool used_sweep_{false};
};

}  // namespace esphome::fram_store
