#pragma once
#include <ArduinoJson.h>
#include <vector>
#include "enums.h"

namespace esphome::automations {

struct TriggerConfig {
  SourceTrigger source = SourceTrigger::NONE;

  union {
    struct {
      TypesInputTrigger type;
      uint32_t input_id;
    } input;

    struct {
      TypesTemperatureTrigger type;
      uint32_t sensor_id;
      float threshold;      // For Below/Above triggers
      float min_threshold;  // For Range trigger
      float max_threshold;  // For Range trigger
    } temperature;

    struct {
      TypesSwitchTrigger type;
      uint32_t switch_id;
    } switch_trigger;

    // Other
  } params;

  // Cron fields (not in union because they use std::vector)
  std::vector<uint8_t> cron_seconds;
  std::vector<uint8_t> cron_minutes;
  std::vector<uint8_t> cron_hours;
  std::vector<uint8_t> cron_days_of_month;
  std::vector<uint8_t> cron_months;
  std::vector<uint8_t> cron_days_of_week;
  CronPreset cron_preset = CronPreset::DAILY;

  TriggerConfig();
  ~TriggerConfig() = default;
  TriggerConfig(const TriggerConfig &other) = default;
  TriggerConfig &operator=(const TriggerConfig &other) = default;

  void serialize(JsonObject &obj) const;
  bool deserialize(const JsonObject &obj);
};

struct ConditionConfig {
  ConditionType type = ConditionType::NONE;

  // For composite conditions (And, Or)
  std::vector<ConditionConfig> sub_conditions;

  // For Input condition
  uint32_t sensor_id = 0;
  InputConditionState state = InputConditionState::TRUE;

  // For Temperature condition
  TypesTemperatureCondition temperature_type = TypesTemperatureCondition::NONE;
  float threshold = 0.0f;      // For Below/Above conditions
  float min_threshold = 0.0f;  // For Range condition
  float max_threshold = 0.0f;  // For Range condition

  ConditionConfig();
  ~ConditionConfig() = default;
  ConditionConfig(const ConditionConfig &other) = default;
  ConditionConfig &operator=(const ConditionConfig &other) = default;

  void serialize(JsonObject &obj) const;
  bool deserialize(const JsonObject &obj);
  bool is_valid() const {
    if (type == ConditionType::NONE)
      return false;
    // A group with no children is degenerate; treat it as "no condition" so the
    // automation runs its actions directly rather than being rejected. A
    // non-empty group whose entities are missing still fails later, in the
    // factory (which returns null and aborts the save).
    if (type == ConditionType::AND || type == ConditionType::OR || type == ConditionType::XOR)
      return !sub_conditions.empty();
    return true;
  }
};

struct ActionConfig {
  SourceAction source = SourceAction::NONE;

  union {
    struct {
      TypeSwitchAction type;
      uint32_t switch_id;
      bool invert;  // "follow" only: drive the target to the opposite of the trigger
    } switch_action;

    struct {
      uint32_t delay_ms;  // the scheduler's own unit; caps the delay at ~49.7 days
    } delay;
  } params;

  ActionConfig();
  ~ActionConfig() = default;
  ActionConfig(const ActionConfig &other) = default;
  ActionConfig &operator=(const ActionConfig &other) = default;

  void serialize(JsonObject &obj) const;
  bool deserialize(const JsonObject &obj);
};

struct AutomationConfig {
  uint32_t id{0};  // Unique ID, assigned by AutomationStorage (0 = not yet assigned)
  std::string name;
  bool enabled = true;
  AutomationMode mode = AutomationMode::SINGLE;
  std::vector<TriggerConfig> triggers;  // Multiple triggers (OR logic)
  ConditionConfig condition;            // Optional condition - check type != None
  std::vector<ActionConfig> actions;
  std::vector<ActionConfig> else_actions;  // Actions to execute when condition is false

  void serialize(JsonObject &obj) const;
  bool deserialize(const JsonObject &obj);
};

class AutomationConfigStorage {
 private:
  std::vector<AutomationConfig> configs_;

 public:
  bool load_from_json(const char *json_str, size_t max_buffer_size);
  bool load_from_json(const JsonArray &array);
  size_t save_to_json(char *json_str, size_t max_buffer_size);

  void add_config(const AutomationConfig &config);
  AutomationConfig *get_config(uint8_t index);
  void update_config(uint8_t index, AutomationConfig *);
  bool remove_config(uint8_t index);
  const std::vector<AutomationConfig> &get_all_configs() const { return configs_; }
  void sort_by_id();
  void clear() { configs_.clear(); }

  size_t size() const { return configs_.size(); }
  bool empty() const { return configs_.empty(); }
};

}  // namespace esphome::automations
