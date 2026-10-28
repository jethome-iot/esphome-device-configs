#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "delay_line.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/config_json/config_json.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "input_file.h"
#ifdef USE_WEBSERVER_SORTING
#include "esphome/components/web_server/web_server.h"
#endif
#ifdef ENTITY_CONFIG_BINARY_SENSOR
#include "esphome/components/entity_config/binary_sensor_settings_json.h"
#endif

namespace esphome::virtual_inputs {

/// One binary sensor of the pool, named "Virtual input N" for good: rules, bindings and Home
/// Assistant know it by that object id whatever its label. A free slot is internal.
class VirtualInput final : public binary_sensor::BinarySensor {
 public:
  explicit VirtualInput(uint8_t index);

  const char *fixed_name() const { return this->name_; }
  /// Visible or internal under the same name; upstream reads the flag when it lists or pushes.
  void set_visible(bool visible, uint32_t entity_fields);

 protected:
  // EntityBase keeps a pointer to the name, not a copy.
  char name_[24]{};
};

/// Whether an object id may be a virtual input's source.
enum class SourceCheck : uint8_t {
  OK,
  /// No visible binary sensor has it.
  MISSING,
  /// A virtual input has it: one never follows another.
  VIRTUAL,
};

/// A pool of virtual inputs: each follows one physical input through an on and an off delay,
/// kept as a file in the config_json keeper's folder. See README.md.
///
/// Everything here runs on the loop task; a web handler hands its calls over.
class VirtualInputs : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the keeper has mounted its folder (HARDWARE + 5) and before entity_config applies the
  // labels and the bindings (HARDWARE + 1): a binding to a slot finds it named and visible.
  float get_setup_priority() const override { return setup_priority::HARDWARE + 2.0f; }

  void set_max_inputs(uint8_t count);
  void set_keeper(config_json::ConfigJsonKeeper *keeper) { this->keeper_ = keeper; }
#ifdef USE_WEBSERVER_SORTING
  void set_web_server_sorting(web_server::WebServer *server, uint64_t group, float weight);
#endif
#ifdef ENTITY_CONFIG_BINARY_SENSOR
  /// Where the slots' labels live; they take no inversion there.
  void set_input_settings(entity_config::BinarySensorSettingsJson *settings) { this->input_settings_ = settings; }
#endif

  size_t max_inputs() const { return this->max_inputs_; }
  /// The slot follows a source; slots are 0-based here.
  bool configured(size_t slot) const;
  /// What the slot follows; an empty source for a free slot or one past the pool.
  const InputConfig &config(size_t slot) const;
  /// The slot's binary sensor, configured or not; nullptr past the pool.
  binary_sensor::BinarySensor *sensor(size_t slot) const;
  bool is_virtual_input(const binary_sensor::BinarySensor *sensor) const;
  /// Why a configured slot is off whatever its source does, or waits; "" when it follows it.
  std::string problem(size_t slot) const;
  /// The lowest free slot, -1 when every one is taken.
  int first_free() const;

  SourceCheck check_source(const std::string &object_id) const;
  /// What a slot may follow: every visible binary sensor that is not a virtual input.
  std::vector<binary_sensor::BinarySensor *> sources() const;

  /// The keeper's folder can be written.
  bool can_save() const;
  /// The file loaded at boot, or there was none; one that did not waits for a person to fix it.
  bool file_loaded() const { return this->file_loaded_; }
  /// Writes the file, then puts the slot in force: shown if it was free, its delays counted
  /// again from now. False, with nothing changed, unless the slot is in the pool, the source
  /// checks OK, both delays are at most MAX_DELAY_S, can_save() and file_loaded() hold and the
  /// write succeeds. The same config again is true with nothing written.
  bool save(size_t slot, const InputConfig &config);
  /// Writes the file without the slot, then hides it with no state and drops its label. False,
  /// with nothing changed, for a free slot or under the conditions save() names.
  bool remove(size_t slot);

 protected:
  struct Slot {
    explicit Slot(uint8_t index) : entity(index) {}
    VirtualInput entity;
    InputConfig config;
    // What config.source resolved to when the slot started; nullptr when it is not a source.
    binary_sensor::BinarySensor *source{nullptr};
    DelayLine line;
  };

  // Seams: the tests set the clock and count the reconnects.
  virtual uint32_t now_ms_() const;
  virtual void resync_home_assistant_();

  void build_pool_();
  void load_();
  binary_sensor::BinarySensor *find_source_(const std::string &object_id) const;
  void subscribe_(binary_sensor::BinarySensor *source);
  void on_source_(binary_sensor::BinarySensor *source, bool level);
  /// Resolves a configured slot's source and takes its level, counting any wait from `now`.
  void start_(Slot *slot, uint32_t now);
  /// What the line shows, when the entity does not show it yet.
  void publish_(Slot *slot);
  bool write_(const std::vector<InputConfig> &table);
  std::vector<InputConfig> table_() const;
  /// Home Assistant lists entities only when it connects. Named, so a burst costs one reconnect.
  void schedule_ha_resync_();

  config_json::ConfigJsonKeeper *keeper_{nullptr};
  InputFile file_{"virtual_inputs"};
  uint8_t max_inputs_{16};
  bool file_loaded_{true};
  std::vector<Slot *> slots_;
  // Sources already carrying a dispatcher: subscribed once each, never removed.
  std::vector<binary_sensor::BinarySensor *> subscribed_;
  uint32_t ha_resync_delay_ms_{2000};
#ifdef USE_WEBSERVER_SORTING
  web_server::WebServer *web_server_{nullptr};
  uint64_t sorting_group_{0};
  float sorting_weight_{50.0f};
#endif
#ifdef ENTITY_CONFIG_BINARY_SENSOR
  entity_config::BinarySensorSettingsJson *input_settings_{nullptr};
#endif
};

}  // namespace esphome::virtual_inputs
