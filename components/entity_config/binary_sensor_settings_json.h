#pragma once

#include "esphome/core/defines.h"
#ifdef ENTITY_CONFIG_BINARY_SENSOR

#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "entity_lookup.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/binary_sensor/filter.h"
#include "esphome/components/config_json/config_json.h"
#include "esphome/components/config_json/settings_base_json.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::entity_config {

// Last filter in the sensor's chain; flipping it re-emits the last raw level, so the change
// shows without waiting for an edge.
class RuntimeInvertFilter : public binary_sensor::Filter {
 public:
  optional<bool> new_value(bool value) override {
    this->raw_ = value;
    return value != this->inverted_;
  }

  void set_inverted(bool inverted) {
    if (this->inverted_ == inverted)
      return;
    this->inverted_ = inverted;
    if (this->raw_.has_value())
      this->output(*this->raw_ != inverted);
  }

  // Appended after the sensor already has a state: what it shows is what this filter would have seen.
  void seed(bool raw) {
    if (!this->raw_.has_value())
      this->raw_ = raw;
  }

  bool is_inverted() const { return this->inverted_; }

 protected:
  bool inverted_{false};
  optional<bool> raw_;
};

struct BinarySensorSettingsRecord {
  std::string source_name_;
  bool inverted = false;

  const char *source_name() const { return this->source_name_.c_str(); }
  uint32_t key() const { return fnv1_hash(this->source_name_); }

  void to_json(JsonObject obj, uint32_t version) const {
    obj["source_name"] = this->source_name_;
    obj["inverted"] = this->inverted;
  }

  bool from_json(JsonObject obj, uint32_t version) {
    if (!obj["source_name"].is<const char *>())
      return false;
    const char *src_name = obj["source_name"];
    if (src_name == nullptr)
      return false;
    this->source_name_ = src_name;
    this->inverted = obj["inverted"] | false;
    return true;
  }
};

class BinarySensorSettingsJson
    : public config_json::SettingsBaseJsonTyped<BinarySensorSettingsJson, BinarySensorSettingsRecord> {
  friend class config_json::SettingsBaseJsonTyped<BinarySensorSettingsJson, BinarySensorSettingsRecord>;

 public:
  static constexpr const char *TAG = "entity_config.binary_sensor";
  static constexpr const char *NAME = "binary_sensor";

  const char *get_key() override { return NAME; }

  // Before the sensors' own setup(), so the filter is in the chain for the initial state.
  static constexpr float APPLY_PRIORITY = setup_priority::HARDWARE + 1.0f;

  BinarySensorSettingsRecord *make_record(binary_sensor::BinarySensor *sensor_obj, bool inverted) {
    if (sensor_obj == nullptr)
      return nullptr;
    const std::string object_id = object_id_of(*sensor_obj);
    BinarySensorSettingsRecord *record = nullptr;
    for (auto *rec : this->records_) {
      if (rec != nullptr && rec->source_name_ == object_id) {
        record = rec;
        break;
      }
    }
    if (record == nullptr) {
      record = new BinarySensorSettingsRecord();  // NOLINT(cppcoreguidelines-owning-memory)
      record->source_name_ = object_id;
      this->records_.push_back(record);
    }
    record->inverted = inverted;
    this->mark_dirty();
    return record;
  }

  bool get_record(binary_sensor::BinarySensor *sensor_obj, BinarySensorSettingsRecord &record) {
    if (sensor_obj == nullptr)
      return false;
    const std::string object_id = object_id_of(*sensor_obj);
    for (const auto *rec : this->records_) {
      if (rec != nullptr && rec->source_name_ == object_id) {
        record = *rec;
        return true;
      }
    }
    return false;
  }

  // REST: {"source_name": ..., "settings": {"inverted": bool}}.
  BinarySensorSettingsRecord *update_record(JsonObject obj) {
    const char *source_name = obj["source_name"];
    if (source_name == nullptr || strlen(source_name) == 0)
      return nullptr;
    auto *sensor = find_binary_sensor(fnv1_hash(source_name));
    if (sensor == nullptr)
      return nullptr;
    JsonObject settings = obj["settings"];
    return this->make_record(sensor, settings["inverted"] | false);
  }

  // Display menu: applied and saved at once. Runs on the loop task.
  bool is_inverted(binary_sensor::BinarySensor *sensor) {
    BinarySensorSettingsRecord record;
    return this->get_record(sensor, record) && record.inverted;
  }

  const char *inverted_label(binary_sensor::BinarySensor *sensor) { return this->is_inverted(sensor) ? "Yes" : "No"; }

  void toggle_inverted(binary_sensor::BinarySensor *sensor) {
    auto *record = this->make_record(sensor, !this->is_inverted(sensor));
    this->apply_record_(record);
    if (config_json::global_config_json_keeper != nullptr)
      config_json::global_config_json_keeper->save(NAME);
  }

  void write_settings_meta(JsonObject obj) override {
    JsonObject inverted_field = obj["inverted"].to<JsonObject>();
    inverted_field["type"] = "boolean";
    inverted_field["label"] = "Inverted";
    inverted_field["description"] = "Reports the input the other way round: a closed contact shows as Off.";
    inverted_field["default"] = false;
  }

 protected:
  RuntimeInvertFilter *filter_for_(binary_sensor::BinarySensor *sensor) {
    for (auto &entry : this->filters_) {
      if (entry.first == sensor)
        return entry.second;
    }
    auto *filter = new RuntimeInvertFilter();  // NOLINT(cppcoreguidelines-owning-memory)
    if (sensor->has_state())
      filter->seed(sensor->state);
    sensor->add_filter(filter);
    this->filters_.emplace_back(sensor, filter);
    return filter;
  }

  void apply_record_(BinarySensorSettingsRecord *record) {
    if (record == nullptr)
      return;
    auto *sensor = find_binary_sensor(record->key());
    if (sensor == nullptr) {
      ESP_LOGW(TAG, "Binary sensor not found for source_name '%s'", record->source_name());
      return;
    }
    this->filter_for_(sensor)->set_inverted(record->inverted);
    ESP_LOGD(TAG, "Applied settings to binary_sensor '%s'", record->source_name());
  }

  std::vector<std::pair<binary_sensor::BinarySensor *, RuntimeInvertFilter *>> filters_;
};

}  // namespace esphome::entity_config

#endif  // ENTITY_CONFIG_BINARY_SENSOR
