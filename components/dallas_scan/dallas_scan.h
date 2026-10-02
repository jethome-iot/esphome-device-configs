#pragma once

#include <string>
#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"
#include "esphome/components/one_wire/one_wire_bus.h"
#include "esphome/components/sensor/sensor.h"
#ifdef USE_SENSOR_FILTER
#include "esphome/components/sensor/filter.h"
#endif
#ifdef USE_WEBSERVER_SORTING
#include "esphome/components/web_server/web_server.h"
#endif
#ifdef USE_DALLAS_SCAN_FILE
#include "esphome/components/config_json/config_json.h"
#include "slot_file.h"
#endif

namespace esphome::dallas_scan {

/// What assign() would do with a slot and an address, asked before it does it.
enum class AssignCheck : uint8_t {
  OK,              ///< the table changes
  BAD_SLOT,        ///< past the end of the table
  BAD_ADDRESS,     ///< not a thermometer ROM with a valid CRC
  LISTED_SLOT,     ///< the slot is taken by sensors:
  LISTED_ADDRESS,  ///< a listed sensor's device, which keeps its own slot
  UNCHANGED,       ///< the device is in that slot already
};

/// One temperature sensor per DS18B20-family device found on the bus at boot.
/// Slot numbers stick: the slot table lives in preferences, or in a file when set_slot_file() is called.
class DallasScan : public PollingComponent {
 public:
  void set_one_wire_bus(one_wire::OneWireBus *bus) { this->bus_ = bus; }
  void set_max_sensors(uint8_t count) {
    this->slots_.assign(count, 0);
    this->given_.assign(count, nullptr);
    this->pinned_.assign(count, false);
#ifdef USE_SENSOR_FILTER
    this->filters_.resize(count);
#endif
  }
  void set_name_prefix(const char *prefix) { this->name_prefix_ = prefix; }
  void set_resolution(uint8_t resolution) { this->resolution_ = resolution; }
  void set_entity_strings(uint8_t device_class_idx, uint8_t uom_idx);
  void set_preference_hash(uint32_t hash) { this->preference_hash_ = hash; }
#ifdef USE_DALLAS_SCAN_FILE
  /// Keep the table in <key>.json in the keeper's folder instead; after set_max_sensors().
  void set_slot_file(config_json::ConfigJsonKeeper *keeper, const char *key);
#endif
  /// The address of a listed 1-Wire sensor: that device keeps the slot.
  void pin(size_t slot, uint64_t address) {
    this->pins_.emplace_back(slot, address);
    this->pinned_[slot] = true;
  }
  /// A YAML sensor from sensors: takes the slot; it reads its device itself, the component only lists it.
  void set_sensor(size_t slot, sensor::Sensor *sensor) {
    this->given_[slot] = sensor;
    this->pinned_[slot] = true;
  }
#ifdef USE_SENSOR_FILTER
  /// The filter chain of a slot's sensor, attached when the sensor is created.
  void set_filters(size_t slot, std::vector<sensor::Filter *> filters) { this->filters_[slot] = std::move(filters); }
#endif
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
  /// Slots up to the last bound one, so a free slot between bound ones is not skipped.
  size_t used_slots() const;
  /// The slot's sensor name, "<prefix> N" when the slot is empty.
  std::string slot_name(size_t slot) const;
  /// Taken by a sensor from sensors:, so forget leaves it alone.
  bool pinned(size_t slot) const { return slot < this->pinned_.size() && this->pinned_[slot]; }
  /// Whether forget(slot) would empty anything: the slot (any slot for -1) holds a device
  /// and is not listed.
  bool can_forget(int slot) const;
  /// False when the table cannot be written (a file whose partition did not mount): forget()
  /// and assign() then change nothing.
  bool can_save() const;
  /// Empty a slot (every slot for -1), then reboot to scan the bus again. Listed slots stay.
  void forget(int slot);
  /// A ROM a slot can hold: a thermometer family, and the CRC the bus scan checks.
  static bool valid_address(uint64_t address);
  AssignCheck check_assign(size_t slot, uint64_t address) const;
  /// Put the device at @p address into @p slot, then reboot. If it held another slot, that slot
  /// takes what @p slot held; otherwise the device @p slot held loses its slot and, still on the
  /// bus, takes the lowest free one at the next boot. Nothing happens unless check_assign() is OK.
  void assign(size_t slot, uint64_t address);

 protected:
  /// Virtual so the host tests can see the reboot: the real one ends the process.
  virtual void restart_();
  void load_table_();
  void bind_devices_();
  sensor::Sensor *make_sensor_(size_t slot);
  void write_resolution_(uint64_t address);
  void read_slot_(size_t slot);
  void update_status_();
  bool read_scratch_pad_(uint64_t address, uint8_t *scratch_pad);
  float to_celsius_(uint64_t address, const uint8_t *scratch_pad) const;
  bool uses_file_() const;
  bool save_table_();
  bool store_for_reboot_();
  void store_and_restart_(const std::vector<uint64_t> &before, const char *outcome);

  one_wire::OneWireBus *bus_{nullptr};
  const char *name_prefix_{"Temp"};
  uint8_t resolution_{12};
  uint16_t conversion_ms_{750};
  uint32_t entity_fields_{0};
  uint32_t preference_hash_{0};
  std::vector<std::pair<size_t, uint64_t>> pins_;
  std::vector<uint64_t> slots_;  // slot -> ROM address, 0 = empty
  std::vector<bool> pinned_;     // slot -> taken by sensors:
#ifdef USE_SENSOR_FILTER
  std::vector<std::vector<sensor::Filter *>> filters_;  // slot -> filter chain
#endif
  std::vector<sensor::Sensor *> given_;    // slot -> YAML sensor from sensors:, nullptr = none
  std::vector<sensor::Sensor *> sensors_;  // slot -> sensor, nullptr = empty
  std::vector<sensor::Sensor *> bound_;    // sensors_ without the gaps
  size_t automatic_{0};                    // slots the component reads itself
  std::vector<bool> missing_;              // slot -> the sensor did not answer the last read
  ESPPreferenceObject pref_;
#ifdef USE_DALLAS_SCAN_FILE
  config_json::ConfigJsonKeeper *keeper_{nullptr};
  SlotFile *file_{nullptr};      // nullptr: the table is in preferences
  bool file_unreadable_{false};  // the file is there but did not load
#endif
#ifdef USE_WEBSERVER_SORTING
  web_server::WebServer *web_server_{nullptr};
  uint64_t sorting_group_{0};
  float sorting_weight_{50};
#endif
};

}  // namespace esphome::dallas_scan
