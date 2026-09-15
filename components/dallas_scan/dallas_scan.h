#pragma once

#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"
#include "esphome/components/one_wire/one_wire_bus.h"
#include "esphome/components/sensor/filter.h"
#include "esphome/components/sensor/sensor.h"
#ifdef USE_WEBSERVER_SORTING
#include "esphome/components/web_server/web_server.h"
#endif

namespace esphome::dallas_scan {

/// One temperature sensor per DS18B20-family device found on the bus at boot.
/// Slot numbers stick: the slot table lives in flash.
class DallasScan : public PollingComponent {
 public:
  void set_one_wire_bus(one_wire::OneWireBus *bus) { this->bus_ = bus; }
  void set_max_sensors(uint8_t count) {
    this->slots_.assign(count, 0);
    this->given_.assign(count, nullptr);
    this->pinned_.assign(count, false);
    this->filters_.resize(count);
  }
  void set_name_prefix(const char *prefix) { this->name_prefix_ = prefix; }
  void set_resolution(uint8_t resolution) { this->resolution_ = resolution; }
  void set_entity_strings(uint8_t device_class_idx, uint8_t uom_idx);
  void set_preference_hash(uint32_t hash) { this->preference_hash_ = hash; }
  /// Pin a slot to a ROM address for good.
  void pin(uint8_t slot, uint64_t address) {
    this->pins_.emplace_back(slot, address);
    this->pinned_[slot] = true;
  }
  /// A YAML sensor from sensors: takes the slot; it reads its device itself, the component only lists it.
  void set_sensor(size_t slot, sensor::Sensor *sensor) {
    this->given_[slot] = sensor;
    this->pinned_[slot] = true;
  }
  /// The filter chain of a slot's sensor, attached when the sensor is created.
  void set_filters(size_t slot, std::vector<sensor::Filter *> filters) { this->filters_[slot] = std::move(filters); }
#ifdef USE_WEBSERVER_SORTING
  void set_web_server_sorting(web_server::WebServer *server, uint64_t group, float weight);
#endif

  float get_setup_priority() const override { return setup_priority::DATA; }
  void setup() override;
  void update() override;
  void dump_config() override;

  size_t max_sensors() const { return this->slots_.size(); }
  /// ROM address in a slot, 0 when the slot is empty.
  uint64_t address(size_t slot) const { return slot < this->slots_.size() ? this->slots_[slot] : 0; }
  /// The slot's sensor, nullptr when the slot is empty.
  sensor::Sensor *sensor(size_t slot) const { return slot < this->sensors_.size() ? this->sensors_[slot] : nullptr; }
  /// The slot's last reading, NAN when the slot is empty or the sensor did not answer.
  float temperature(size_t slot) const;
  /// Sensors of the bound slots, in slot order.
  const std::vector<sensor::Sensor *> &sensors() const { return this->bound_; }
  /// Fixed by the config (a pinned address or a YAML sensor): forget leaves it alone.
  bool pinned(size_t slot) const { return slot < this->pinned_.size() && this->pinned_[slot]; }
  /// Empty a slot (every slot for -1), then reboot to scan the bus again. Pinned slots stay.
  void forget(int slot);

 protected:
  void bind_devices_();
  sensor::Sensor *make_sensor_(size_t slot);
  void write_resolution_(uint64_t address);
  void read_slot_(size_t slot);
  void update_status_();
  bool read_scratch_pad_(uint64_t address, uint8_t *scratch_pad);
  float to_celsius_(uint64_t address, const uint8_t *scratch_pad) const;
  void save_table_();

  one_wire::OneWireBus *bus_{nullptr};
  const char *name_prefix_{"Temp"};
  uint8_t resolution_{12};
  uint32_t entity_fields_{0};
  uint32_t preference_hash_{0};
  std::vector<std::pair<uint8_t, uint64_t>> pins_;
  std::vector<uint64_t> slots_;            // slot -> ROM address, 0 = empty
  std::vector<bool> pinned_;               // slot -> fixed by the config
  std::vector<std::vector<sensor::Filter *>> filters_;  // slot -> filter chain
  std::vector<sensor::Sensor *> given_;    // slot -> YAML sensor from sensors:, nullptr = none
  std::vector<sensor::Sensor *> sensors_;  // slot -> sensor, nullptr = empty
  std::vector<sensor::Sensor *> bound_;    // sensors_ without the gaps
  std::vector<bool> missing_;              // slot -> the sensor did not answer the last read
  ESPPreferenceObject pref_;
#ifdef USE_WEBSERVER_SORTING
  web_server::WebServer *web_server_{nullptr};
  uint64_t sorting_group_{0};
  float sorting_weight_{50};
#endif
};

}  // namespace esphome::dallas_scan
