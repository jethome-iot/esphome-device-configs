#include "dallas_scan.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::dallas_scan {

static const char *const TAG = "dallas_scan";

// ROM families with the DS18B20 scratch pad layout.
static const uint8_t FAMILIES[] = {0x10, 0x22, 0x28, 0x3b, 0x42};
static const uint8_t FAMILY_DS18S20 = 0x10;
// Conversion time by resolution, 9 to 12 bits.
static const uint16_t CONVERSION_MS[] = {94, 188, 375, 750};

static const uint8_t CMD_START_CONVERSION = 0x44;
static const uint8_t CMD_WRITE_SCRATCH_PAD = 0x4E;
static const uint8_t CMD_COPY_SCRATCH_PAD = 0x48;
static const uint8_t CMD_READ_SCRATCH_PAD = 0xBE;

static constexpr int16_t MAX_OFFSET_TENTHS = (int16_t) (DallasScan::MAX_OFFSET * 10);

static bool is_temperature_sensor(uint64_t address) {
  return std::find(std::begin(FAMILIES), std::end(FAMILIES), address & 0xff) != std::end(FAMILIES);
}

bool offset_tenths(double celsius, int16_t &tenths) {
  // Bounded first, so lround() stays in range.
  if (!std::isfinite(celsius) || std::fabs(celsius) > 2 * DallasScan::MAX_OFFSET)
    return false;
  // The margin covers float and JSON parser error (under 3e-6 tenths within the range), so 0.15
  // rounds to 0.2 however it was stored, while 0.04999 still rounds to 0.0.
  const long rounded = std::lround(celsius * 10.0 + std::copysign(1e-5, celsius));
  if (std::labs(rounded) > MAX_OFFSET_TENTHS)
    return false;
  tenths = (int16_t) rounded;
  return true;
}

void DallasScan::set_entity_strings(uint8_t device_class_idx, uint8_t uom_idx) {
  this->entity_fields_ =
      ((uint32_t) device_class_idx << ENTITY_FIELD_DC_SHIFT) | ((uint32_t) uom_idx << ENTITY_FIELD_UOM_SHIFT);
}

#ifdef USE_DALLAS_SCAN_FILE
void DallasScan::set_slot_file(config_json::ConfigJsonKeeper *keeper, const char *key) {
  this->keeper_ = keeper;
  // Not one of the keeper's settings types: those are entity settings, served by the dashboard.
  this->file_ = new SlotFile(key, this->slots_.size());  // NOLINT(cppcoreguidelines-owning-memory)
}
#endif

#ifdef USE_WEBSERVER_SORTING
void DallasScan::set_web_server_sorting(web_server::WebServer *server, uint64_t group, float weight) {
  this->web_server_ = server;
  this->sorting_group_ = group;
  this->sorting_weight_ = weight;
}
#endif

void DallasScan::setup() {
  this->load_table_();
  // Listed slots have filters of their own; a stored offset of theirs goes at the next write.
  for (size_t slot = 0; slot < this->offsets_.size(); slot++) {
    if (this->pinned_[slot])
      this->offsets_[slot] = 0;
  }
  this->bind_devices_();
  this->booted_ = this->slots_;

  this->sensors_.assign(this->slots_.size(), nullptr);
  this->missing_.assign(this->slots_.size(), false);
  const auto &devices = this->bus_->get_devices();
  for (size_t slot = 0; slot < this->slots_.size(); slot++) {
    if (auto *given = this->given_[slot]; given != nullptr) {
      this->sensors_[slot] = given;
      this->bound_.push_back(given);
      continue;
    }
    const uint64_t address = this->slots_[slot];
    if (address == 0)
      continue;
    auto *sensor = this->make_sensor_(slot);
    this->sensors_[slot] = sensor;
    this->bound_.push_back(sensor);
    this->automatic_++;
    if (std::find(devices.begin(), devices.end(), address) == devices.end()) {
      ESP_LOGW(TAG, "%s: 0x%016" PRIx64 " is not on the bus", sensor->get_name().c_str(), address);
      this->missing_[slot] = true;
      continue;
    }
    this->write_resolution_(address);
  }
  // One wait for the whole bus: a DS18S20 ignores the resolution and takes the full 750 ms.
  this->conversion_ms_ = CONVERSION_MS[this->resolution_ - 9];
  for (size_t slot = 0; slot < this->slots_.size(); slot++) {
    if (this->given_[slot] == nullptr && (this->slots_[slot] & 0xff) == FAMILY_DS18S20 && this->slots_[slot] != 0)
      this->conversion_ms_ = CONVERSION_MS[3];
  }
  this->update_status_();
}

void DallasScan::load_table_() {
#ifdef USE_DALLAS_SCAN_FILE
  if (this->file_ != nullptr) {
    // The keeper has set up by now: no mount means it failed.
    if (this->can_save())
      this->file_unreadable_ =
          !this->file_->load_from_file(this->keeper_->get_storage(), this->keeper_->get_config_dir());
    this->slots_ = this->file_->table();
    this->offsets_ = this->file_->offsets();
    for (auto &address : this->slots_) {
      if (address != 0 && !is_temperature_sensor(address)) {
        ESP_LOGW(TAG, "Not a temperature sensor, dropping 0x%016" PRIx64 " from the table", address);
        address = 0;
      }
    }
    return;
  }
#endif
  const size_t bytes = this->slots_.size() * sizeof(uint64_t);
  this->pref_ = global_preferences->make_preference(bytes, this->preference_hash_);
  if (!this->pref_.load(reinterpret_cast<uint8_t *>(this->slots_.data()), bytes))
    std::fill(this->slots_.begin(), this->slots_.end(), 0);
  // A record of its own, so a table stored by older firmware still loads; sized by max_sensors too.
  const size_t offset_bytes = this->offsets_.size() * sizeof(int16_t);
  this->offsets_pref_ =
      global_preferences->make_preference(offset_bytes, fnv1_hash_extend(this->preference_hash_, "offsets"));
  if (!this->offsets_pref_.load(reinterpret_cast<uint8_t *>(this->offsets_.data()), offset_bytes))
    std::fill(this->offsets_.begin(), this->offsets_.end(), 0);
  for (size_t slot = 0; slot < this->offsets_.size(); slot++) {
    if (std::abs(this->offsets_[slot]) > MAX_OFFSET_TENTHS) {
      ESP_LOGW(TAG, "Slot %u: the stored offset is out of range, dropping it", (unsigned) slot + 1);
      this->offsets_[slot] = 0;
    }
  }
}

void DallasScan::bind_devices_() {
  const auto before = this->slots_;
  const auto begin = this->slots_.begin(), end = this->slots_.end();
  // A slot taken by a YAML sensor holds that sensor's address, if any, and nothing else.
  for (size_t slot = 0; slot < this->slots_.size(); slot++) {
    if (this->given_[slot] != nullptr)
      this->slots_[slot] = 0;
  }
  // A listed sensor's address owns its slot; the stored table follows.
  for (const auto &[slot, address] : this->pins_) {
    if (slot >= this->slots_.size())
      continue;
    std::replace(begin, end, address, (uint64_t) 0);
    this->slots_[slot] = address;
  }
  for (uint64_t address : this->bus_->get_devices()) {
    if (!is_temperature_sensor(address)) {
      ESP_LOGW(TAG, "Not a temperature sensor, skipping 0x%016" PRIx64, address);
      continue;
    }
    if (std::find(begin, end, address) != end)
      continue;
    size_t slot = 0;
    while (slot < this->slots_.size() && (this->slots_[slot] != 0 || this->given_[slot] != nullptr))
      slot++;
    if (slot == this->slots_.size()) {
      ESP_LOGW(TAG, "No free slot for 0x%016" PRIx64, address);
      continue;
    }
    this->slots_[slot] = address;
    ESP_LOGI(TAG, "0x%016" PRIx64 " takes slot %u", address, (unsigned) slot + 1);
  }
  if (this->slots_ == before)
    return;
#ifdef USE_DALLAS_SCAN_FILE
  // A file that did not load stays as it is, for a person to fix; only a forget or an assign
  // writes over it.
  if (this->file_ != nullptr && this->file_unreadable_) {
    ESP_LOGW(TAG, "%s.json did not load: these slots last until the next reboot", this->file_->get_key());
    return;
  }
#endif
  this->save_table_();
}

sensor::Sensor *DallasScan::make_sensor_(size_t slot) {
  // The entity refers to its name rather than copying it, so both live until reboot.
  auto *sensor = new sensor::Sensor();             // NOLINT(cppcoreguidelines-owning-memory)
  const std::string text = this->slot_name(slot);  // still empty here, so "<prefix> N"
  auto *name = new char[text.size() + 1];          // NOLINT(cppcoreguidelines-owning-memory)
  memcpy(name, text.c_str(), text.size() + 1);
  sensor->set_accuracy_decimals(1);
  sensor->set_state_class(sensor::STATE_CLASS_MEASUREMENT);
#ifdef USE_SENSOR_FILTER
  for (auto *filter : this->filters_[slot])
    sensor->add_filter(filter);
#endif
  const size_t count = App.get_sensors().size();
  App.register_sensor(sensor, name, 0, this->entity_fields_);  // hash 0: derived from the name, as codegen does
  if (App.get_sensors().size() == count)
    ESP_LOGE(TAG, "%s: no room in the entity table", name);
#ifdef USE_WEBSERVER_SORTING
  if (this->web_server_ != nullptr)
    this->web_server_->add_entity_config(sensor, this->sorting_weight_ + slot, this->sorting_group_);
#endif
  return sensor;
}

void DallasScan::write_resolution_(uint64_t address) {
  if ((address & 0xff) == FAMILY_DS18S20)
    return;  // fixed 9 bits
  uint8_t scratch_pad[9];
  if (!this->read_scratch_pad_(address, scratch_pad))
    return;
  const uint8_t config = 0x1F | ((this->resolution_ - 9) << 5);
  if (scratch_pad[4] == config)
    return;
  if (!this->bus_->select(address))
    return;
  this->bus_->write8(CMD_WRITE_SCRATCH_PAD);
  this->bus_->write8(scratch_pad[2]);  // alarm high
  this->bus_->write8(scratch_pad[3]);  // alarm low
  this->bus_->write8(config);
  if (this->bus_->select(address))
    this->bus_->write8(CMD_COPY_SCRATCH_PAD);
}

void DallasScan::update() {
  // Listed sensors read their devices themselves.
  if (this->automatic_ == 0)
    return;
  // One conversion for the whole bus; the scratch pads are read one per loop pass.
  if (this->bus_->skip())
    this->bus_->write8(CMD_START_CONVERSION);
  this->set_timeout("read", this->conversion_ms_, [this] { this->read_slot_(0); });
}

void DallasScan::read_slot_(size_t slot) {
  // Slots served by YAML sensors are theirs to read.
  while (slot < this->slots_.size() && (this->sensors_[slot] == nullptr || this->given_[slot] != nullptr))
    slot++;
  if (slot >= this->slots_.size()) {
    this->update_status_();
    return;
  }
  auto *sensor = this->sensors_[slot];
  // The boot table, not the saved one: a sensor keeps its device until the reboot.
  const uint64_t address = this->booted_[slot];
  uint8_t scratch_pad[9];
  // A sensor that stops answering is logged once, not on every read.
  if (!this->read_scratch_pad_(address, scratch_pad)) {
    if (!this->missing_[slot]) {
      ESP_LOGW(TAG, "%s: 0x%016" PRIx64 " does not answer", sensor->get_name().c_str(), address);
      this->missing_[slot] = true;
      this->raw_[slot] = NAN;
      sensor->publish_state(NAN);
    }
  } else {
    if (this->missing_[slot]) {
      ESP_LOGI(TAG, "%s: 0x%016" PRIx64 " answers again", sensor->get_name().c_str(), address);
      this->missing_[slot] = false;
      this->write_resolution_(address);
    }
    const float celsius = this->to_celsius_(address, scratch_pad);
    if (celsius != 85.0f) {  // power-on value, not a reading
      this->raw_[slot] = celsius;
      sensor->publish_state(celsius + this->offset(slot));
    }
  }
  this->set_timeout("read", 0, [this, slot] { this->read_slot_(slot + 1); });
}

// A new offset shows at once instead of at the next poll.
void DallasScan::republish_(size_t slot) {
  if (!std::isnan(this->raw_[slot]))
    this->sensors_[slot]->publish_state(this->raw_[slot] + this->offset(slot));
}

void DallasScan::update_status_() {
  if (std::any_of(this->missing_.begin(), this->missing_.end(), [](bool missing) { return missing; })) {
    this->status_set_warning("a sensor does not answer");
  } else {
    this->status_clear_warning();
  }
}

bool DallasScan::read_scratch_pad_(uint64_t address, uint8_t *scratch_pad) {
  if (!this->bus_->select(address)) {
    ESP_LOGV(TAG, "0x%016" PRIx64 ": bus reset failed", address);
    return false;
  }
  this->bus_->write8(CMD_READ_SCRATCH_PAD);
  for (size_t i = 0; i < 9; i++)
    scratch_pad[i] = this->bus_->read8();
  if (crc8(scratch_pad, 8) != scratch_pad[8]) {
    ESP_LOGV(TAG, "0x%016" PRIx64 ": scratch pad checksum invalid", address);
    return false;
  }
  return true;
}

float DallasScan::to_celsius_(uint64_t address, const uint8_t *scratch_pad) const {
  int16_t raw = (scratch_pad[1] << 8) | scratch_pad[0];
  if ((address & 0xff) == FAMILY_DS18S20) {
    if (scratch_pad[7] == 0)
      return NAN;
    // The shift is the datasheet's TEMP_READ: bit 0 dropped, a floor for negative values too.
    return (raw >> 1) + (scratch_pad[7] - scratch_pad[6]) / float(scratch_pad[7]) - 0.25f;
  }
  raw &= ~((1 << (12 - this->resolution_)) - 1);  // undefined low bits below the resolution
  return raw / 16.0f;
}

size_t DallasScan::used_slots() const {
  size_t used = 0;
  for (size_t slot = 0; slot < this->sensors_.size(); slot++) {
    if (this->sensors_[slot] != nullptr)
      used = slot + 1;
  }
  return used;
}

size_t DallasScan::saved_slots() const {
  size_t used = 0;
  for (size_t slot = 0; slot < this->slots_.size(); slot++) {
    if (this->slots_[slot] != 0)
      used = slot + 1;
  }
  return used;
}

std::string DallasScan::slot_name(size_t slot) const {
  if (auto *sensor = this->sensor(slot); sensor != nullptr)
    return sensor->get_name().c_str();
  return str_sprintf("%s %u", this->name_prefix_, (unsigned) slot + 1);
}

float DallasScan::temperature(size_t slot) const {
  auto *sensor = this->sensor(slot);
  return sensor == nullptr ? NAN : sensor->state;
}

bool DallasScan::can_forget(int slot) const {
  for (size_t i = 0; i < this->slots_.size(); i++) {
    // Forget all clears the offsets too, so an offset alone is something to forget.
    const bool held = this->slots_[i] != 0 || (slot < 0 && this->offsets_[i] != 0);
    if ((slot < 0 || (size_t) slot == i) && !this->pinned_[i] && held)
      return true;
  }
  return false;
}

void DallasScan::forget(int slot) {
  // The dashboard changed this since boot: the panel's Confirm applies what it saved instead of erasing it.
  const bool saved_already = slot < 0 ? !this->can_forget(slot) && this->reboot_required() : this->slot_pending(slot);
  if (saved_already && this->can_save()) {
    ESP_LOGI(TAG, "Rebooting to apply the saved slot table");
    this->restart_();
    return;
  }
  if (this->forget_and_save(slot))
    this->restart_();
}

bool DallasScan::forget_and_save(int slot) {
  if (!this->can_save()) {
    ESP_LOGE(TAG, "Storage unavailable: nothing is forgotten");
    return false;
  }
  if (!this->can_forget(slot)) {
    ESP_LOGW(TAG, "Nothing to forget: the slot is empty or taken by a YAML sensor");
    return false;
  }
  const auto before = this->slots_;
  const auto offsets = this->offsets_;
  for (size_t i = 0; i < this->slots_.size(); i++) {
    if ((slot < 0 || (size_t) slot == i) && !this->pinned_[i]) {
      this->slots_[i] = 0;
      // Forget all numbers the devices again, so the offsets would land on other sensors.
      if (slot < 0)
        this->offsets_[i] = 0;
    }
  }
  // In preferences these are two records, and a flush writes each on its own. The offsets go
  // first: zeros on flash land on no wrong sensor, a new table with the old offsets would.
  auto keep = offsets;
  if (!this->uses_file_() && this->offsets_ != offsets) {
    if (!this->store_offsets_now_()) {
      this->slots_ = before;
      this->offsets_ = offsets;
      ESP_LOGE(TAG, "The offsets were not written: nothing is forgotten");
      return false;
    }
    keep = this->offsets_;  // on flash already, so a table that fails keeps them
  }
  const bool stored = this->store_or_roll_back_(
      before, keep, keep == offsets ? "nothing is forgotten" : "only the offsets are cleared");
  for (size_t i = 0; i < this->offsets_.size(); i++) {
    if (this->offsets_[i] != offsets[i])
      this->republish_(i);
  }
  return stored;
}

bool DallasScan::valid_address(uint64_t address) {
  // Byte 0 is the family and byte 7 the CRC of the other seven, as the bus reads them.
  const auto *rom = reinterpret_cast<const uint8_t *>(&address);
  return is_temperature_sensor(address) && crc8(rom, 7) == rom[7];
}

AssignCheck DallasScan::check_assign(size_t slot, uint64_t address) const {
  if (slot >= this->slots_.size())
    return AssignCheck::BAD_SLOT;
  if (!valid_address(address))
    return AssignCheck::BAD_ADDRESS;
  if (this->pinned_[slot])
    return AssignCheck::LISTED_SLOT;
  // Boot puts a listed sensor's device back in its own slot, whatever the table says.
  for (const auto &pin : this->pins_) {
    if (pin.second == address)
      return AssignCheck::LISTED_ADDRESS;
  }
  if (this->slots_[slot] == address)
    return AssignCheck::UNCHANGED;
  return AssignCheck::OK;
}

void DallasScan::assign(size_t slot, uint64_t address) {
  if (this->assign_and_save(slot, address))
    this->restart_();
}

bool DallasScan::assign_and_save(size_t slot, uint64_t address) {
  if (this->check_assign(slot, address) != AssignCheck::OK) {
    ESP_LOGW(TAG, "Not assigning 0x%016" PRIx64 " to slot %u", address, (unsigned) slot + 1);
    return false;
  }
  if (!this->can_save()) {
    ESP_LOGE(TAG, "Storage unavailable: nothing is assigned");
    return false;
  }
  const auto before = this->slots_;
  auto held = std::find(this->slots_.begin(), this->slots_.end(), address);
  if (held != this->slots_.end())
    *held = this->slots_[slot];
  this->slots_[slot] = address;
  if (!this->store_or_roll_back_(before, this->offsets_, "nothing is assigned"))
    return false;
  ESP_LOGI(TAG, "0x%016" PRIx64 " takes slot %u%s", address, (unsigned) slot + 1,
           this->slot_pending(slot) ? " after a reboot" : "");
  return true;
}

OffsetCheck DallasScan::check_offset(size_t slot, float value) const {
  int16_t tenths;
  return this->check_offset_(slot, value, tenths);
}

OffsetCheck DallasScan::check_offset_(size_t slot, float value, int16_t &tenths) const {
  if (slot >= this->slots_.size())
    return OffsetCheck::BAD_SLOT;
  if (!offset_tenths(value, tenths))
    return OffsetCheck::BAD_VALUE;
  if (this->pinned_[slot])
    return OffsetCheck::LISTED_SLOT;
  return OffsetCheck::OK;
}

bool DallasScan::set_offset_and_save(size_t slot, float value) {
  int16_t tenths = 0;
  if (this->check_offset_(slot, value, tenths) != OffsetCheck::OK) {
    ESP_LOGW(TAG, "Not setting the offset of slot %u to %.2f", (unsigned) slot + 1, value);
    return false;
  }
  if (this->offsets_[slot] == tenths)
    return true;
  if (!this->can_set_offset()) {
    ESP_LOGE(TAG, "%s: the offset is not changed",
             this->can_save() ? "The slot file did not load" : "Storage unavailable");
    return false;
  }
  const auto offsets = this->offsets_;
  this->offsets_[slot] = tenths;
  if (!this->store_or_roll_back_(this->slots_, offsets, "the offset is not changed"))
    return false;
  ESP_LOGI(TAG, "%s: offset %+.1f °C", this->slot_name(slot).c_str(), this->offset(slot));
  this->republish_(slot);
  return true;
}

// The next boot would bring the old table back without a word, so memory follows storage.
bool DallasScan::store_or_roll_back_(const std::vector<uint64_t> &before, const std::vector<int16_t> &offsets,
                                     const char *outcome) {
  if (this->store_now_()) {
    this->reboot_required_.store(this->slots_ != this->booted_);
    return true;
  }
  this->slots_ = before;
  this->offsets_ = offsets;
  ESP_LOGE(TAG, "The slot table was not written: %s", outcome);
  return false;
}

void DallasScan::restart_() { App.safe_reboot(); }

// The caller is told the change is saved, so the table has to be on flash now, not queued.
bool DallasScan::store_now_() {
  if (!this->save_table_())
    return false;
  if (this->uses_file_() || global_preferences->sync())
    return true;
  // The flush reports for every record at once, so the failure may be another one's. It has
  // emptied the queue, so this reads what flash holds.
  std::vector<uint64_t> stored(this->slots_.size(), 0);
  std::vector<int16_t> offsets(this->offsets_.size(), 0);
  if (this->pref_.load(reinterpret_cast<uint8_t *>(stored.data()), stored.size() * sizeof(uint64_t)) &&
      stored == this->slots_ &&
      this->offsets_pref_.load(reinterpret_cast<uint8_t *>(offsets.data()), offsets.size() * sizeof(int16_t)) &&
      offsets == this->offsets_) {
    ESP_LOGW(TAG, "Flushing flash failed for another record; the slot table is stored all the same");
    return true;
  }
  return false;
}

// The offsets record alone, flushed and checked as store_now_() checks both.
bool DallasScan::store_offsets_now_() {
  const size_t bytes = this->offsets_.size() * sizeof(int16_t);
  if (!this->offsets_pref_.save(reinterpret_cast<const uint8_t *>(this->offsets_.data()), bytes))
    return false;
  if (global_preferences->sync())
    return true;
  std::vector<int16_t> stored(this->offsets_.size(), 0);
  return this->offsets_pref_.load(reinterpret_cast<uint8_t *>(stored.data()), bytes) && stored == this->offsets_;
}

bool DallasScan::uses_file_() const {
#ifdef USE_DALLAS_SCAN_FILE
  return this->file_ != nullptr;
#else
  return false;
#endif
}

bool DallasScan::can_save() const {
#ifdef USE_DALLAS_SCAN_FILE
  if (this->file_ != nullptr)
    return this->keeper_->can_save();
#endif
  return true;
}

bool DallasScan::can_set_offset() const {
#ifdef USE_DALLAS_SCAN_FILE
  if (this->file_ != nullptr && this->file_unreadable_)
    return false;
#endif
  return this->can_save();
}

bool DallasScan::save_table_() {
#ifdef USE_DALLAS_SCAN_FILE
  if (this->file_ != nullptr) {
    if (!this->can_save()) {
      ESP_LOGE(TAG, "Storage unavailable: the slots last until the next reboot");
      return false;
    }
    this->file_->set_table(this->slots_);
    this->file_->set_offsets(this->offsets_);
    this->keeper_->ensure_config_dir();
    return this->file_->save_to_file(this->keeper_->get_storage(), this->keeper_->get_config_dir());
  }
#endif
  const size_t bytes = this->slots_.size() * sizeof(uint64_t);
  const size_t offset_bytes = this->offsets_.size() * sizeof(int16_t);
  if (!this->pref_.save(reinterpret_cast<const uint8_t *>(this->slots_.data()), bytes) ||
      !this->offsets_pref_.save(reinterpret_cast<const uint8_t *>(this->offsets_.data()), offset_bytes)) {
    ESP_LOGE(TAG, "Saving the slot table failed");
    return false;
  }
  return true;
}

void DallasScan::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Dallas scan:\n"
                "  Slots: %u\n"
                "  Resolution: %u bits",
                (unsigned) this->slots_.size(), this->resolution_);
#ifdef USE_DALLAS_SCAN_FILE
  if (this->file_ != nullptr)
    ESP_LOGCONFIG(TAG, "  Slot file: %s/%s.json", this->keeper_->get_config_dir().c_str(), this->file_->get_key());
#endif
  LOG_UPDATE_INTERVAL(this);
  for (size_t slot = 0; slot < this->slots_.size(); slot++) {
    auto *sensor = this->sensors_[slot];
    if (sensor == nullptr)
      continue;
    if (this->given_[slot] != nullptr) {
      ESP_LOGCONFIG(TAG, "  %s: YAML sensor in slot %u", sensor->get_name().c_str(), (unsigned) slot + 1);
      continue;
    }
    ESP_LOGCONFIG(TAG, "  %s: 0x%016" PRIx64 " (%s)", sensor->get_name().c_str(), this->booted_[slot],
                  LOG_STR_ARG(this->bus_->get_model_str(this->booted_[slot] & 0xff)));
  }
  for (size_t slot = 0; slot < this->offsets_.size(); slot++) {
    if (this->offsets_[slot] != 0)
      ESP_LOGCONFIG(TAG, "  %s offset: %+.1f °C", this->slot_name(slot).c_str(), this->offset(slot));
  }
}

}  // namespace esphome::dallas_scan
