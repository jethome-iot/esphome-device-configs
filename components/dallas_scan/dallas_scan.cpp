#include "dallas_scan.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
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

static bool is_temperature_sensor(uint64_t address) {
  return std::find(std::begin(FAMILIES), std::end(FAMILIES), address & 0xff) != std::end(FAMILIES);
}

void DallasScan::set_entity_strings(uint8_t device_class_idx, uint8_t uom_idx) {
  this->entity_fields_ =
      ((uint32_t) device_class_idx << ENTITY_FIELD_DC_SHIFT) | ((uint32_t) uom_idx << ENTITY_FIELD_UOM_SHIFT);
}

#ifdef USE_WEBSERVER_SORTING
void DallasScan::set_web_server_sorting(web_server::WebServer *server, uint64_t group, float weight) {
  this->web_server_ = server;
  this->sorting_group_ = group;
  this->sorting_weight_ = weight;
}
#endif

void DallasScan::setup() {
  const size_t bytes = this->slots_.size() * sizeof(uint64_t);
  this->pref_ = global_preferences->make_preference(bytes, this->preference_hash_);
  if (!this->pref_.load(reinterpret_cast<uint8_t *>(this->slots_.data()), bytes))
    std::fill(this->slots_.begin(), this->slots_.end(), 0);

  this->bind_devices_();

  this->sensors_.assign(this->slots_.size(), nullptr);
  for (size_t slot = 0; slot < this->slots_.size(); slot++) {
    if (this->slots_[slot] == 0)
      continue;
    this->sensors_[slot] = this->make_sensor_(slot);
    this->bound_.push_back(this->sensors_[slot]);
    this->write_resolution_(this->slots_[slot]);
  }
}

void DallasScan::bind_devices_() {
  const auto before = this->slots_;
  const auto begin = this->slots_.begin(), end = this->slots_.end();
  // A pinned address owns its slot; the stored table follows.
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
    auto slot = std::find(begin, end, (uint64_t) 0);
    if (slot == end) {
      ESP_LOGW(TAG, "No free slot for 0x%016" PRIx64, address);
      continue;
    }
    *slot = address;
    ESP_LOGI(TAG, "0x%016" PRIx64 " takes slot %u", address, (unsigned) (slot - begin) + 1);
  }
  if (this->slots_ != before)
    this->save_table_();
}

sensor::Sensor *DallasScan::make_sensor_(size_t slot) {
  // The entity refers to its name rather than copying it, so both live until reboot.
  auto *sensor = new sensor::Sensor();  // NOLINT(cppcoreguidelines-owning-memory)
  const size_t len = strlen(this->name_prefix_) + 4;
  auto *name = new char[len];  // NOLINT(cppcoreguidelines-owning-memory)
  snprintf(name, len, "%s%u", this->name_prefix_, (unsigned) slot + 1);
  sensor->set_accuracy_decimals(1);
  sensor->set_state_class(sensor::STATE_CLASS_MEASUREMENT);
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
  const auto &devices = this->bus_->get_devices();
  if (std::find(devices.begin(), devices.end(), address) == devices.end())
    return;  // not on the bus right now
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
  if (this->bound_.empty())
    return;
  this->status_clear_warning();
  // One conversion for the whole bus; the scratch pads are read one per loop pass.
  if (this->bus_->skip())
    this->bus_->write8(CMD_START_CONVERSION);
  this->set_timeout("read", CONVERSION_MS[this->resolution_ - 9], [this] { this->read_slot_(0); });
}

void DallasScan::read_slot_(size_t slot) {
  while (slot < this->slots_.size() && this->sensors_[slot] == nullptr)
    slot++;
  if (slot >= this->slots_.size())
    return;
  uint8_t scratch_pad[9];
  if (!this->read_scratch_pad_(this->slots_[slot], scratch_pad)) {
    this->status_set_warning();
    this->sensors_[slot]->publish_state(NAN);
  } else {
    const float celsius = this->to_celsius_(this->slots_[slot], scratch_pad);
    if (celsius != 85.0f)  // power-on value, not a reading
      this->sensors_[slot]->publish_state(celsius);
  }
  this->set_timeout("read", 0, [this, slot] { this->read_slot_(slot + 1); });
}

bool DallasScan::read_scratch_pad_(uint64_t address, uint8_t *scratch_pad) {
  if (!this->bus_->select(address)) {
    ESP_LOGW(TAG, "0x%016" PRIx64 ": bus reset failed", address);
    return false;
  }
  this->bus_->write8(CMD_READ_SCRATCH_PAD);
  for (size_t i = 0; i < 9; i++)
    scratch_pad[i] = this->bus_->read8();
  if (crc8(scratch_pad, 8) != scratch_pad[8]) {
    ESP_LOGW(TAG, "0x%016" PRIx64 ": scratch pad checksum invalid", address);
    return false;
  }
  return true;
}

float DallasScan::to_celsius_(uint64_t address, const uint8_t *scratch_pad) const {
  int16_t raw = (scratch_pad[1] << 8) | scratch_pad[0];
  if ((address & 0xff) == FAMILY_DS18S20) {
    if (scratch_pad[7] == 0)
      return NAN;
    return (raw >> 1) + (scratch_pad[7] - scratch_pad[6]) / float(scratch_pad[7]) - 0.25f;
  }
  raw &= ~((1 << (12 - this->resolution_)) - 1);  // undefined low bits below the resolution
  return raw / 16.0f;
}

float DallasScan::temperature(size_t slot) const {
  auto *sensor = this->sensor(slot);
  return sensor == nullptr ? NAN : sensor->state;
}

void DallasScan::forget(int slot) {
  for (size_t i = 0; i < this->slots_.size(); i++) {
    if (slot < 0 || (size_t) slot == i)
      this->slots_[i] = 0;
  }
  this->save_table_();
  global_preferences->sync();
  App.safe_reboot();
}

void DallasScan::save_table_() {
  const size_t bytes = this->slots_.size() * sizeof(uint64_t);
  if (!this->pref_.save(reinterpret_cast<const uint8_t *>(this->slots_.data()), bytes))
    ESP_LOGE(TAG, "Saving the slot table failed");
}

void DallasScan::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Dallas scan:\n"
                "  Slots: %u\n"
                "  Resolution: %u bits",
                (unsigned) this->slots_.size(), this->resolution_);
  LOG_UPDATE_INTERVAL(this);
  for (size_t slot = 0; slot < this->slots_.size(); slot++) {
    if (this->slots_[slot] == 0)
      continue;
    ESP_LOGCONFIG(TAG, "  %s: 0x%016" PRIx64 " (%s)", this->sensors_[slot]->get_name().c_str(), this->slots_[slot],
                  LOG_STR_ARG(this->bus_->get_model_str(this->slots_[slot] & 0xff)));
  }
}

}  // namespace esphome::dallas_scan
