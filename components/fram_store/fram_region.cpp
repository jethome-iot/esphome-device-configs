#include "fram_region.h"
#include <algorithm>
#include <cinttypes>
#include <cstring>
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "fram_store.h"

namespace esphome::fram_store {

static const char *const TAG = "fram_store";

// Buffer used for streaming CRCs and for scanning ring headers -- sized to stay off the heap.
static const size_t CHUNK = 64;
static const size_t SCAN_BUFFER = 128;

// How many damaged records a ring tolerates before it is wiped: one power cut leaves one, and a ring whose
// head cannot be found would silently lose every later append.
static const uint16_t MAX_TORN_RECORDS = 4;
// One more candidate than that, so the valid head behind four damaged records is still reachable.
static const uint8_t HEAD_CANDIDATES = MAX_TORN_RECORDS + 1;

static_assert(sizeof(SlotHeader) == FramSlot::HEADER_SIZE, "slot header size drifted from the on-device layout");
static_assert(sizeof(RingHeader) == FramRing::HEADER_SIZE, "ring header size drifted from the on-device layout");

// A sequence number of 0 marks an unwritten record, so wrap-around skips it.
static uint32_t next_seq(uint32_t seq) { return seq + 1 == 0 ? 1 : seq + 1; }

static bool seq_is_newer(uint32_t candidate, uint32_t current) { return static_cast<int32_t>(candidate - current) > 0; }

// CRC over a header up to but not including its own crc field.
template<typename T> static uint16_t header_crc(const T &header) {
  return crc16(reinterpret_cast<const uint8_t *>(&header), static_cast<uint16_t>(sizeof(T) - sizeof(header.crc)));
}

bool FramRegion::read_(uint32_t offset, uint8_t *data, size_t len) {
  return this->in_region_(offset, len) && this->store_->read(this->offset_ + offset, data, len);
}

bool FramRegion::write_(uint32_t offset, const uint8_t *data, size_t len) {
  return this->in_region_(offset, len) && this->store_->write(this->offset_ + offset, data, len);
}

bool FramRegion::fill_(uint32_t offset, uint8_t value, size_t len) {
  return this->in_region_(offset, len) && this->store_->fill(this->offset_ + offset, value, len);
}

// Every access stays inside the region, so a bad offset cannot reach a neighbour or the superblock.
bool FramRegion::in_region_(uint32_t offset, size_t len) const {
  if (offset <= this->size_ && len <= this->size_ - offset)
    return true;
  ESP_LOGE(TAG, "%zu bytes at +%" PRIu32 " run past the %" PRIu32 "-byte region at 0x%04X", len, offset, this->size_,
           this->offset_);
  return false;
}

bool FramRegion::wipe_() { return this->fill_(0, 0, this->size_); }

bool FramRegion::crc_of_range_(uint32_t offset, size_t len, uint16_t &crc) {
  uint8_t buffer[CHUNK];

  while (len != 0) {
    size_t chunk = std::min(len, CHUNK);
    if (!this->read_(offset, buffer, chunk))
      return false;
    crc = crc16(buffer, static_cast<uint16_t>(chunk), crc);
    offset += chunk;
    len -= chunk;
  }

  return true;
}

// A read failure leaves the slot uninitialised on purpose: saving into a copy whose sequence number is
// unknown would let an older copy win on the next boot.
void FramSlot::init() {
  this->initialised_ = false;
  this->has_data_ = false;
  this->load_failed_ = false;
  this->active_ = 0;
  this->seq_ = 0;
  this->highest_seq_ = 0;

  SlotHeader header_a;
  SlotHeader header_b;
  if (!this->read_header_(0, header_a) || !this->read_header_(1, header_b)) {
    ESP_LOGE(TAG, "Slot at 0x%04X could not be read", this->offset_);
    return;
  }

  CheckResult copy_a = this->validate_copy_(0, header_a);
  CheckResult copy_b = this->validate_copy_(1, header_b);
  if (copy_a == CheckResult::IO_ERROR || copy_b == CheckResult::IO_ERROR) {
    ESP_LOGE(TAG, "Slot at 0x%04X could not be read", this->offset_);
    return;
  }

  this->initialised_ = true;
  this->highest_seq_ = seq_is_newer(header_b.seq, header_a.seq) ? header_b.seq : header_a.seq;

  bool ok_a = copy_a == CheckResult::OK;
  bool ok_b = copy_b == CheckResult::OK;
  if (!ok_a && !ok_b)
    return;

  if (ok_a && ok_b) {
    this->active_ = seq_is_newer(header_b.seq, header_a.seq) ? 1 : 0;
  } else {
    this->active_ = ok_b ? 1 : 0;
  }

  this->has_data_ = true;
  this->seq_ = this->active_ == 1 ? header_b.seq : header_a.seq;
}

bool FramSlot::read_header_(uint8_t copy, SlotHeader &header) {
  return this->read_(this->copy_offset_(copy), reinterpret_cast<uint8_t *>(&header), sizeof(header));
}

CheckResult FramSlot::validate_copy_(uint8_t copy, const SlotHeader &header) {
  if (header.seq == 0 || header.len != this->payload_size_)
    return CheckResult::DAMAGED;

  uint16_t crc = header_crc(header);
  if (!this->crc_of_range_(this->copy_offset_(copy) + HEADER_SIZE, this->payload_size_, crc))
    return CheckResult::IO_ERROR;

  return crc == header.crc ? CheckResult::OK : CheckResult::DAMAGED;
}

CheckResult FramSlot::read_copy_(uint8_t copy, uint8_t *data, uint32_t &seq) {
  SlotHeader header;
  if (!this->read_header_(copy, header) ||
      !this->read_(this->copy_offset_(copy) + HEADER_SIZE, data, this->payload_size_))
    return CheckResult::IO_ERROR;

  uint16_t crc = crc16(data, this->payload_size_, header_crc(header));
  if (header.seq == 0 || header.len != this->payload_size_ || crc != header.crc)
    return CheckResult::DAMAGED;

  seq = header.seq;
  return CheckResult::OK;
}

// Re-validates on the way out: init() only proves the copy was good at boot, not that it still is. A
// damaged newest copy falls back to the one before it, as a boot would; an unreadable one does not, since
// the newer value may well be intact.
bool FramSlot::load(uint8_t *data, size_t len) {
  if (!this->initialised_ || !this->has_data_ || len != this->payload_size_)
    return false;

  uint32_t seq = 0;
  CheckResult active = this->read_copy_(this->active_, data, seq);
  if (active == CheckResult::OK) {
    this->load_failed_ = false;
    return true;
  }

  const uint8_t other = 1 - this->active_;
  if (active == CheckResult::DAMAGED && this->read_copy_(other, data, seq) == CheckResult::OK) {
    ESP_LOGW(TAG, "Slot at 0x%04X: the newest copy is damaged, loaded the one before it", this->offset_);
    this->active_ = other;
    this->seq_ = seq;
    this->load_failed_ = false;
    return true;
  }

  ESP_LOGE(TAG, "Slot at 0x%04X could not be loaded; saving is refused until a load succeeds or clear()",
           this->offset_);
  this->load_failed_ = true;
  return false;
}

// Payload first, header last: an interrupted save leaves the previous copy as the newest valid one.
bool FramSlot::save(const uint8_t *data, size_t len) {
  if (!this->initialised_) {
    ESP_LOGE(TAG, "Slot at 0x%04X is not initialised", this->offset_);
    return false;
  }
  if (this->load_failed_) {
    ESP_LOGE(TAG, "Slot at 0x%04X failed its last load, refusing to save over it", this->offset_);
    return false;
  }
  if (len != this->payload_size_) {
    ESP_LOGE(TAG, "Slot at 0x%04X takes %u bytes, got %zu", this->offset_, this->payload_size_, len);
    return false;
  }

  uint8_t target = this->has_data_ ? 1 - this->active_ : 0;
  uint32_t base = this->copy_offset_(target);

  SlotHeader header;
  header.seq = next_seq(this->highest_seq_);
  header.len = this->payload_size_;
  header.crc = crc16(data, static_cast<uint16_t>(len), header_crc(header));

  if (!this->write_(base + HEADER_SIZE, data, len))
    return false;
  if (!this->write_(base, reinterpret_cast<const uint8_t *>(&header), sizeof(header)))
    return false;

  this->active_ = target;
  this->seq_ = header.seq;
  this->highest_seq_ = header.seq;
  this->has_data_ = true;
  return true;
}

// Refuses on an uninitialised region for the same reason save() does: its contents are unknown. The older
// copy goes first, so a clear cut short leaves the current value, never the one before it.
bool FramSlot::clear() {
  if (!this->initialised_)
    return false;

  const size_t copy_size = HEADER_SIZE + this->payload_size_;
  if (!this->fill_(this->copy_offset_(1 - this->active_), 0, copy_size) ||
      !this->fill_(this->copy_offset_(this->active_), 0, copy_size))
    return false;

  this->has_data_ = false;
  this->load_failed_ = false;
  this->active_ = 0;
  this->seq_ = 0;
  this->highest_seq_ = 0;
  return true;
}

void FramRing::init() {
  this->initialised_ = false;
  this->head_index_ = 0;
  this->head_seq_ = 0;
  this->count_ = 0;
  this->used_sweep_ = false;

  uint16_t index = 0;
  uint32_t seq = 0;
  uint16_t count = 0;
  CheckResult located = this->locate_head_(index, seq, count);
  if (located == CheckResult::IO_ERROR) {
    ESP_LOGE(TAG, "Ring at 0x%04X could not be read", this->offset_);
    return;
  }

  if (located == CheckResult::OK) {
    uint32_t validated = 0;
    if (this->validate_entry_(index, validated) == CheckResult::OK && this->head_is_consistent_(index, seq)) {
      this->initialised_ = true;
      this->head_index_ = index;
      this->head_seq_ = validated;
      this->count_ = count;
      this->used_sweep_ = false;
      return;
    }
  }

  // The fast path assumes undamaged records; anything else falls back to reading the whole ring.
  this->initialised_ = false;
  this->head_index_ = 0;
  this->head_seq_ = 0;
  this->count_ = 0;
  this->used_sweep_ = true;

  uint16_t indices[HEAD_CANDIDATES] = {};
  uint32_t seqs[HEAD_CANDIDATES] = {};
  uint16_t found = 0;
  if (!this->scan_(indices, seqs, HEAD_CANDIDATES, found)) {
    ESP_LOGE(TAG, "Ring at 0x%04X could not be read", this->offset_);
    return;
  }

  this->initialised_ = true;
  if (found == 0)
    return;

  // One sweep collects the newest few records, so a damaged head costs a validation, not another sweep.
  for (uint8_t candidate = 0; candidate < HEAD_CANDIDATES && seqs[candidate] != 0; candidate++) {
    uint32_t validated = 0;
    CheckResult result = this->validate_entry_(indices[candidate], validated);
    if (result == CheckResult::IO_ERROR) {
      ESP_LOGE(TAG, "Ring at 0x%04X could not be read", this->offset_);
      this->initialised_ = false;
      return;
    }
    if (result == CheckResult::OK) {
      this->head_index_ = indices[candidate];
      this->head_seq_ = validated;
      this->count_ = found > candidate ? static_cast<uint16_t>(found - candidate) : 0;
      return;
    }
  }

  // No usable head: keeping the records would make every later append invisible, so start the ring over.
  ESP_LOGW(TAG, "Ring at 0x%04X: the newest %u record(s) are all damaged, wiping it", this->offset_,
           std::min<uint16_t>(found, HEAD_CANDIDATES));
  if (!this->wipe_()) {
    this->initialised_ = false;
    return;
  }
  this->head_index_ = 0;
  this->head_seq_ = 0;
  this->count_ = 0;
}

bool FramRing::read_seq_(uint16_t index, uint32_t &seq) {
  RingHeader header;
  if (!this->read_(this->entry_offset_(index), reinterpret_cast<uint8_t *>(&header), sizeof(header)))
    return false;

  seq = header.seq;
  return true;
}

// In a full ring the sequence numbers rise with the index and wrap at most once, so the head is a binary
// search over headers: ~11 reads instead of every record, measured as 7 ms against 190 ms on hardware.
CheckResult FramRing::locate_head_(uint16_t &index, uint32_t &seq, uint16_t &count) {
  index = 0;
  seq = 0;
  count = 0;

  uint32_t first = 0;
  uint32_t last = 0;
  if (!this->read_seq_(0, first) || !this->read_seq_(this->capacity_ - 1, last))
    return CheckResult::IO_ERROR;

  // Index 0 is written before any other, so a zero there is either an empty ring or a damaged record --
  // and only the sweep can tell those apart. Guessing "empty" would hide every surviving record.
  if (first == 0)
    return CheckResult::DAMAGED;

  // A blank tail and a hole punched in the middle look the same until the rest is read, which is the
  // sweep -- so only a full ring gets the fast path, and a ring earns it by wrapping once.
  if (last == 0)
    return CheckResult::DAMAGED;

  uint16_t lo = 0;
  uint16_t hi = static_cast<uint16_t>(this->capacity_ - 1);

  if (seq_is_newer(last, first)) {
    index = hi;  // full and never wrapped
    count = this->capacity_;
  } else {
    // Wrapped: the head sits just before the first record older than index 0.
    while (lo + 1 < hi) {
      uint16_t mid = lo + (hi - lo) / 2;
      uint32_t mid_seq = 0;
      if (!this->read_seq_(mid, mid_seq))
        return CheckResult::IO_ERROR;
      if (mid_seq == 0 || seq_is_newer(first, mid_seq)) {
        hi = mid;
      } else {
        lo = mid;
      }
    }
    index = lo;
    count = this->capacity_;
  }

  return this->read_seq_(index, seq) ? CheckResult::OK : CheckResult::IO_ERROR;
}

// Only a full ring gets here, so what follows the head is the oldest record, exactly capacity - 1 behind.
// "Merely older" would accept a head the search reached by mistake, losing everything newer.
bool FramRing::head_is_consistent_(uint16_t index, uint32_t seq) {
  uint32_t next = 0;
  if (!this->read_seq_(static_cast<uint16_t>((index + 1) % this->capacity_), next))
    return false;

  return next != 0 && next == seq - this->capacity_ + 1;
}

// Reads headers in batches where a batch fits, so a full ring costs tens of transfers rather than hundreds,
// and keeps the newest `candidates` of them so one sweep serves every retry.
bool FramRing::scan_(uint16_t *indices, uint32_t *seqs, uint8_t candidates, uint16_t &count) {
  uint8_t buffer[SCAN_BUFFER];
  uint16_t entry = this->entry_size_();
  uint16_t per_read = entry <= SCAN_BUFFER ? SCAN_BUFFER / entry : 0;

  count = 0;

  for (uint16_t index = 0; index < this->capacity_;) {
    uint16_t batch = per_read == 0 ? 1 : std::min<uint16_t>(per_read, this->capacity_ - index);

    if (per_read == 0) {
      RingHeader header;
      if (!this->read_(this->entry_offset_(index), reinterpret_cast<uint8_t *>(&header), sizeof(header)))
        return false;
      std::memcpy(buffer, &header, sizeof(header));
    } else if (!this->read_(this->entry_offset_(index), buffer, static_cast<size_t>(batch) * entry)) {
      return false;
    }

    for (uint16_t k = 0; k < batch; k++) {
      uint32_t seq;
      std::memcpy(&seq, buffer + (per_read == 0 ? 0 : static_cast<size_t>(k) * entry), sizeof(seq));
      if (seq == 0)
        continue;

      count++;
      for (uint8_t slot = 0; slot < candidates; slot++) {
        if (seqs[slot] != 0 && !seq_is_newer(seq, seqs[slot]))
          continue;
        for (uint8_t shift = candidates - 1; shift > slot; shift--) {
          seqs[shift] = seqs[shift - 1];
          indices[shift] = indices[shift - 1];
        }
        seqs[slot] = seq;
        indices[slot] = static_cast<uint16_t>(index + k);
        break;
      }
    }

    index += batch;
  }

  return true;
}

CheckResult FramRing::validate_entry_(uint16_t index, uint32_t &seq) {
  RingHeader header;
  uint32_t base = this->entry_offset_(index);

  if (!this->read_(base, reinterpret_cast<uint8_t *>(&header), sizeof(header)))
    return CheckResult::IO_ERROR;
  if (header.seq == 0)
    return CheckResult::DAMAGED;

  uint16_t crc = header_crc(header);
  if (!this->crc_of_range_(base + HEADER_SIZE, this->record_size_, crc))
    return CheckResult::IO_ERROR;
  if (crc != header.crc)
    return CheckResult::DAMAGED;

  seq = header.seq;
  return CheckResult::OK;
}

bool FramRing::append(const uint8_t *data, size_t len) {
  if (!this->initialised_) {
    ESP_LOGE(TAG, "Ring at 0x%04X is not initialised", this->offset_);
    return false;
  }
  if (len != this->record_size_) {
    ESP_LOGE(TAG, "Ring at 0x%04X takes %u-byte records, got %zu", this->offset_, this->record_size_, len);
    return false;
  }

  uint16_t index = this->count_ == 0 ? 0 : static_cast<uint16_t>((this->head_index_ + 1) % this->capacity_);
  uint32_t base = this->entry_offset_(index);

  RingHeader header;
  header.seq = next_seq(this->head_seq_);
  header.crc = crc16(data, static_cast<uint16_t>(len), header_crc(header));

  if (!this->write_(base + HEADER_SIZE, data, len))
    return false;
  if (!this->write_(base, reinterpret_cast<const uint8_t *>(&header), sizeof(header)))
    return false;

  this->head_index_ = index;
  this->head_seq_ = header.seq;
  if (this->count_ < this->capacity_)
    this->count_++;
  return true;
}

// The sequence check ties a record to its distance from the head. It rejects all but the newest for one lap
// after the 32-bit sequence wraps -- 2^32 appends away, and it costs reads, never data.
bool FramRing::peek(uint16_t index, uint8_t *data, size_t len) {
  if (!this->initialised_ || index >= this->count_ || len != this->record_size_)
    return false;

  uint16_t phys = (this->head_index_ + this->capacity_ - index) % this->capacity_;
  uint32_t base = this->entry_offset_(phys);

  RingHeader header;
  if (!this->read_(base, reinterpret_cast<uint8_t *>(&header), sizeof(header)))
    return false;
  if (!this->read_(base + HEADER_SIZE, data, len))
    return false;

  uint16_t crc = crc16(data, static_cast<uint16_t>(len), header_crc(header));
  return crc == header.crc && header.seq == this->head_seq_ - index;
}

// Physically, everything past the head is older than everything up to it, so two fills go oldest to newest.
bool FramRing::clear() {
  if (!this->initialised_)
    return false;

  const uint32_t past_head = this->entry_offset_(this->head_index_ + 1);
  if (!this->fill_(past_head, 0, this->size_ - past_head) || !this->fill_(0, 0, past_head))
    return false;

  this->head_index_ = 0;
  this->head_seq_ = 0;
  this->count_ = 0;
  return true;
}

}  // namespace esphome::fram_store
