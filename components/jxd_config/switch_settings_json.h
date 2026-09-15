#pragma once

#include "esphome/core/defines.h"
#ifdef JXD_CONFIG_SWITCH

#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#include "entity_lookup.h"
#include "esphome/components/config_json/config_json.h"
#include "esphome/components/config_json/settings_base_json.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef JXD_CONFIG_BINDINGS
#include "esphome/components/bindings/bindings.h"
#endif

namespace esphome::jxd_config {

struct RestoreModeName {
  switch_::SwitchRestoreMode mode;
  const char *name;
};

inline constexpr RestoreModeName RESTORE_MODE_NAMES[] = {
    {switch_::SWITCH_ALWAYS_OFF, "ALWAYS_OFF"},
    {switch_::SWITCH_ALWAYS_ON, "ALWAYS_ON"},
    {switch_::SWITCH_RESTORE_DEFAULT_OFF, "RESTORE_DEFAULT_OFF"},
    {switch_::SWITCH_RESTORE_DEFAULT_ON, "RESTORE_DEFAULT_ON"},
    {switch_::SWITCH_RESTORE_INVERTED_DEFAULT_OFF, "RESTORE_INVERTED_DEFAULT_OFF"},
    {switch_::SWITCH_RESTORE_INVERTED_DEFAULT_ON, "RESTORE_INVERTED_DEFAULT_ON"},
    {switch_::SWITCH_RESTORE_DISABLED, "RESTORE_DISABLED"},
};

inline const char *restore_mode_to_string(switch_::SwitchRestoreMode mode) {
  for (const auto &entry : RESTORE_MODE_NAMES) {
    if (entry.mode == mode)
      return entry.name;
  }
  return "RESTORE_DEFAULT_OFF";
}

inline switch_::SwitchRestoreMode parse_restore_mode(const char *name) {
  if (name != nullptr) {
    for (const auto &entry : RESTORE_MODE_NAMES) {
      if (strcmp(entry.name, name) == 0)
        return entry.mode;
    }
  }
  return switch_::SWITCH_RESTORE_DEFAULT_OFF;
}

// What the dashboard form and the display menu offer, in this order.
struct StartModeOption {
  switch_::SwitchRestoreMode mode;
  const char *label;
};

inline constexpr StartModeOption START_MODE_OPTIONS[] = {
    {switch_::SWITCH_ALWAYS_OFF, "Off"},
    {switch_::SWITCH_ALWAYS_ON, "On"},
    {switch_::SWITCH_RESTORE_DEFAULT_OFF, "Last"},
};

struct BindingModeOption {
  const char *value;
  const char *label;
};

inline constexpr BindingModeOption BINDING_MODE_OPTIONS[] = {
    {"none", "Disabled"},  // not "None": in the form it sits right under the input's None
    {"toggle", "Toggle"},
    {"follow", "Follow"},
};

// Wraps around; from a value the list does not hold, next lands on the first and prev on the last.
inline size_t step_option(int index, int step, size_t count) {
  if (index < 0)
    return step > 0 ? 0 : count - 1;
  const int n = static_cast<int>(count);
  return static_cast<size_t>(((index + step) % n + n) % n);
}

struct SwitchSettingsRecord {
  std::string source_name_;
  switch_::SwitchRestoreMode restore_mode = switch_::SWITCH_ALWAYS_OFF;
  bool inverted = false;
  // Plain strings, so a build without the bindings component round-trips them.
  std::string binding_input;
  std::string binding_mode{"none"};

  const char *source_name() const { return this->source_name_.c_str(); }
  uint32_t key() const { return fnv1_hash(this->source_name_); }

  void to_json(JsonObject obj, uint32_t version) const {
    obj["source_name"] = this->source_name_;
    obj["restore_mode"] = restore_mode_to_string(this->restore_mode);
    obj["inverted"] = this->inverted;
    obj["binding_input"] = this->binding_input;
    obj["binding_mode"] = this->binding_mode;
  }

  bool from_json(JsonObject obj, uint32_t version) {
    if (!obj["source_name"].is<const char *>())
      return false;
    const char *src_name = obj["source_name"];
    if (src_name == nullptr)
      return false;
    this->source_name_ = src_name;
    this->restore_mode = parse_restore_mode(obj["restore_mode"] | "RESTORE_DEFAULT_OFF");
    this->inverted = obj["inverted"] | false;
    this->binding_input = obj["binding_input"] | "";
    this->binding_mode = obj["binding_mode"] | "none";
    return true;
  }
};

class SwitchSettingsJson : public config_json::SettingsBaseJsonTyped<SwitchSettingsJson, SwitchSettingsRecord> {
  friend class config_json::SettingsBaseJsonTyped<SwitchSettingsJson, SwitchSettingsRecord>;

 public:
  static constexpr const char *TAG = "jxd_config.switch";
  static constexpr const char *NAME = "switch";

  const char *get_key() override { return NAME; }

  // Before the switches' own setup(), so restore_mode and inverted are in place for it.
  static constexpr float APPLY_PRIORITY = setup_priority::HARDWARE + 1.0f;

  void apply() override {
    SettingsBaseJsonTyped::apply();
    this->live_ = true;
  }

  SwitchSettingsRecord *make_record(switch_::Switch *switch_obj, switch_::SwitchRestoreMode restore_mode,
                                    bool inverted) {
    if (switch_obj == nullptr)
      return nullptr;
    auto *record = this->find_or_create_(object_id_of(*switch_obj));
    record->restore_mode = restore_mode;
    record->inverted = inverted;
    this->mark_dirty();
    return record;
  }

#ifdef JXD_CONFIG_BINDINGS
  // Only the binding half, so a read-modify-write of restore_mode/inverted cannot reset it.
  SwitchSettingsRecord *make_binding_record(switch_::Switch *switch_obj, const std::string &binding_input,
                                            bindings::BindingMode binding_mode) {
    if (switch_obj == nullptr)
      return nullptr;
    // Stored as chosen: a named input under mode none is inactive, not cleared.
    auto *record = this->find_or_create_(object_id_of(*switch_obj));
    record->binding_input = binding_input;
    record->binding_mode = bindings::binding_mode_to_string(binding_mode);
    this->mark_dirty();
    return record;
  }
#endif

  bool get_record(switch_::Switch *switch_obj, SwitchSettingsRecord &record) {
    if (switch_obj == nullptr)
      return false;
    auto *found = this->find_(object_id_of(*switch_obj));
    if (found == nullptr)
      return false;
    record = *found;
    return true;
  }

  // REST: {"source_name": ..., "settings": {"restore_mode", "inverted", "binding_input", "binding_mode"}}.
  SwitchSettingsRecord *update_record(JsonObject obj) {
    const char *source_name = obj["source_name"];
    if (source_name == nullptr || strlen(source_name) == 0)
      return nullptr;
    auto *sw = find_switch(fnv1_hash(source_name));
    if (sw == nullptr)
      return nullptr;

    JsonObject settings = obj["settings"];
    const bool inverted = settings["inverted"] | false;
    const auto restore_mode = parse_restore_mode(settings["restore_mode"] | "ALWAYS_OFF");

#ifdef JXD_CONFIG_BINDINGS
    // Each key is independently optional; absent means "leave that half alone".
    auto input_value = settings["binding_input"];
    auto mode_value = settings["binding_mode"];
    const bool has_input_key = !input_value.isNull();
    const bool has_mode_key = !mode_value.isNull();
    const bool has_binding_keys = has_input_key || has_mode_key;
    if ((has_input_key && !input_value.is<const char *>()) || (has_mode_key && !mode_value.is<const char *>()))
      return nullptr;

    SwitchSettingsRecord current;
    const bool have_current = this->get_record(sw, current);
    std::string binding_input = has_input_key ? std::string(input_value.as<const char *>())
                                              : (have_current ? current.binding_input : std::string());
    std::string binding_mode_name = has_mode_key ? std::string(mode_value.as<const char *>())
                                                 : (have_current ? current.binding_mode : std::string("none"));

    bindings::BindingMode binding_mode = bindings::BindingMode::NONE;
    if (has_binding_keys) {
      // Only what changed is validated: a stored binding may name an entity this build lacks,
      // and echoing it back must not block edits to the rest of the record.
      const bool input_changed = !have_current || current.binding_input != binding_input;
      const bool mode_changed = !have_current || current.binding_mode != binding_mode_name;
      if (!bindings::parse_binding_mode(binding_mode_name.c_str(), binding_mode) && mode_changed)
        return nullptr;
      if (input_changed && !binding_input.empty() && find_binary_sensor(fnv1_hash(binding_input)) == nullptr)
        return nullptr;
    }
#endif

    auto *record = this->make_record(sw, restore_mode, inverted);
#ifdef JXD_CONFIG_BINDINGS
    if (record != nullptr && has_binding_keys)
      record = this->make_binding_record(sw, binding_input, binding_mode);
#endif
    return record;
  }

  // Display menu: one field at a time, applied and saved at once. Runs on the loop task.
  const char *inverted_label(switch_::Switch *sw) { return this->effective_(sw).inverted ? "Yes" : "No"; }

  void toggle_inverted(switch_::Switch *sw) {
    auto *record = this->edit_(sw);
    record->inverted = !record->inverted;
    this->commit_(record);
  }

  const char *restore_mode_label(switch_::Switch *sw) {
    const auto mode = this->effective_(sw).restore_mode;
    for (const auto &option : START_MODE_OPTIONS) {
      if (option.mode == mode)
        return option.label;
    }
    return restore_mode_to_string(mode);  // a hand-edited file may hold one the form does not offer
  }

  void cycle_restore_mode(switch_::Switch *sw, int step) {
    auto *record = this->edit_(sw);
    int index = -1;
    for (size_t i = 0; i < std::size(START_MODE_OPTIONS); i++) {
      if (START_MODE_OPTIONS[i].mode == record->restore_mode)
        index = i;
    }
    record->restore_mode = START_MODE_OPTIONS[step_option(index, step, std::size(START_MODE_OPTIONS))].mode;
    this->commit_(record);
  }

  std::string binding_input_label(switch_::Switch *sw) {
    const std::string input = this->effective_(sw).binding_input;
    if (input.empty())
      return "None";
    auto *sensor = find_binary_sensor(fnv1_hash(input));
    return sensor != nullptr ? sensor->get_name().str() : input;  // missing in this build: the stored id
  }

  void cycle_binding_input(switch_::Switch *sw, int step) {
    // None first, then the inputs in registration order, as the form lists them.
    std::vector<std::string> options{""};
    for (auto *sensor : App.get_binary_sensors()) {
      if (sensor != nullptr && !sensor->is_internal())
        options.push_back(object_id_of(*sensor));
    }
    auto *record = this->edit_(sw);
    int index = -1;
    for (size_t i = 0; i < options.size(); i++) {
      if (options[i] == record->binding_input)
        index = i;
    }
    record->binding_input = options[step_option(index, step, options.size())];
    this->commit_(record);
  }

  std::string binding_mode_label(switch_::Switch *sw) {
    const std::string mode = this->effective_(sw).binding_mode;
    for (const auto &option : BINDING_MODE_OPTIONS) {
      if (mode == option.value)
        return option.label;
    }
    return mode;  // a hand-edited file may hold one the form does not offer
  }

  void cycle_binding_mode(switch_::Switch *sw, int step) {
    auto *record = this->edit_(sw);
    int index = -1;
    for (size_t i = 0; i < std::size(BINDING_MODE_OPTIONS); i++) {
      if (record->binding_mode == BINDING_MODE_OPTIONS[i].value)
        index = i;
    }
    record->binding_mode = BINDING_MODE_OPTIONS[step_option(index, step, std::size(BINDING_MODE_OPTIONS))].value;
    this->commit_(record);
  }

  void write_settings_meta(JsonObject obj) override {
    JsonObject start_field = obj["restore_mode"].to<JsonObject>();
    start_field["type"] = "enum";
    start_field["label"] = "Start Mode";
    start_field["description"] = "What this output does at power-up: force Off, force On, or restore the state it "
                                 "had before the reboot.";
    start_field["default"] = "ALWAYS_OFF";
    JsonArray options = start_field["options"].to<JsonArray>();
    for (const auto &option : START_MODE_OPTIONS)
      add_option_(options, restore_mode_to_string(option.mode), option.label);

#ifdef JXD_CONFIG_BINDINGS
    // With no bindable input both fields would offer a single choice each.
    if (has_bindable_input_()) {
      JsonObject input_field = obj["binding_input"].to<JsonObject>();
      input_field["type"] = "enum";
      input_field["label"] = "Bound input";
      input_field["description"] = "An input that drives this output directly, with no automation.";
      input_field["default"] = "";
      JsonArray input_options = input_field["options"].to<JsonArray>();
      add_option_(input_options, "", "None");
      for (auto *sensor : App.get_binary_sensors()) {
        if (sensor == nullptr || sensor->is_internal())
          continue;
        JsonObject opt = input_options.add<JsonObject>();
        opt["value"] = object_id_of(*sensor);
        opt["label"] = sensor->get_name().str();
      }

      JsonObject mode_field = obj["binding_mode"].to<JsonObject>();
      mode_field["type"] = "enum";
      mode_field["label"] = "Binding mode";
      mode_field["description"] = "Toggle flips the output on each press of the input; Follow makes the output copy "
                                  "the input and takes over from Start Mode.";
      mode_field["default"] = "none";
      JsonArray mode_options = mode_field["options"].to<JsonArray>();
      for (const auto &option : BINDING_MODE_OPTIONS)
        add_option_(mode_options, option.value, option.label);
    }
#endif

    JsonObject inverted_field = obj["inverted"].to<JsonObject>();
    inverted_field["type"] = "boolean";
    inverted_field["label"] = "Inverted";
    inverted_field["description"] = "Swaps the physical output: the app's On drives the pin low, Off drives it high.";
    inverted_field["default"] = false;
  }

 protected:
  static void add_option_(JsonArray options, const char *value, const char *label) {
    JsonObject opt = options.add<JsonObject>();
    opt["value"] = value;
    opt["label"] = label;
  }

#ifdef JXD_CONFIG_BINDINGS
  static bool has_bindable_input_() {
    for (auto *sensor : App.get_binary_sensors()) {
      if (sensor != nullptr && !sensor->is_internal())
        return true;
    }
    return false;
  }
#endif

  SwitchSettingsRecord *find_(const std::string &object_id) {
    for (auto *record : this->records_) {
      if (record != nullptr && record->source_name_ == object_id)
        return record;
    }
    return nullptr;
  }

  SwitchSettingsRecord *find_or_create_(const std::string &object_id) {
    auto *record = this->find_(object_id);
    if (record == nullptr) {
      record = new SwitchSettingsRecord();  // NOLINT(cppcoreguidelines-owning-memory)
      record->source_name_ = object_id;
      this->records_.push_back(record);
    }
    return record;
  }

  // The stored record, or what the switch runs with when there is none.
  SwitchSettingsRecord effective_(switch_::Switch *sw) {
    SwitchSettingsRecord record;
    if (!this->get_record(sw, record)) {
      record.restore_mode = sw->restore_mode;
      record.inverted = sw->is_inverted();
    }
    return record;
  }

  // The record to edit, seeded from the switch so a first edit changes only its own field.
  SwitchSettingsRecord *edit_(switch_::Switch *sw) {
    const std::string object_id = object_id_of(*sw);
    auto *record = this->find_(object_id);
    if (record == nullptr) {
      record = this->find_or_create_(object_id);
      record->restore_mode = sw->restore_mode;
      record->inverted = sw->is_inverted();
    }
    return record;
  }

  void commit_(SwitchSettingsRecord *record) {
    this->mark_dirty();
    this->apply_record_(record);
    if (config_json::global_config_json_keeper != nullptr)
      config_json::global_config_json_keeper->save(NAME);
  }

  void apply_record_(SwitchSettingsRecord *record) {
    if (record == nullptr)
      return;
    const uint32_t hash = record->key();
    auto *sw = find_switch(hash);
    if (sw == nullptr) {
      ESP_LOGW(TAG, "Switch not found for source_name '%s'", record->source_name());
      return;
    }

    // Persistence switched on at run time starts saving from the next boot: the switch only
    // creates its preference when it boots in a RESTORE_* mode.
    sw->set_restore_mode(record->restore_mode);
    sw->set_inverted(record->inverted);
    // At boot the switch's own setup() drives the pin; later it has to be re-driven so a
    // flipped `inverted` shows on the hardware.
    if (this->live_) {
      if (sw->state) {
        sw->turn_on();
      } else {
        sw->turn_off();
      }
    }

#ifdef JXD_CONFIG_BINDINGS
    if (bindings::global_bindings_manager != nullptr) {
      bindings::BindingMode mode = bindings::BindingMode::NONE;
      if (!bindings::parse_binding_mode(record->binding_mode.c_str(), mode)) {
        // Only a hand-edited file gets here; the REST path rejects the write.
        ESP_LOGW(TAG, "Unknown binding mode '%s' for '%s', leaving it unbound", record->binding_mode.c_str(),
                 record->source_name());
      }
      const uint32_t input_key = record->binding_input.empty() ? 0 : fnv1_hash(record->binding_input);
      bindings::global_bindings_manager->set_binding(hash, input_key, mode);
    }
#endif

    ESP_LOGD(TAG, "Applied settings to switch '%s'", record->source_name());
  }

  bool live_{false};
};

}  // namespace esphome::jxd_config

#endif  // JXD_CONFIG_SWITCH
