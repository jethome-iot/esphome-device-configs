#include "automation_config.h"
#include <algorithm>
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "entity_lookup.h"
#include "esphome/core/log.h"

namespace esphome::automations {

static const char *const TAG = "automations";

// UINT32_MAX is the scheduler's SCHEDULER_DONT_RUN sentinel: set_timeout with it cancels the
// pending item instead of scheduling one, which would strand the run forever. Stay one below.
static constexpr uint32_t MAX_DELAY_MS = SCHEDULER_DONT_RUN - 1;

// Safe integer parsing without exceptions - returns true if parsing succeeded
static bool safe_parse_uint8(const std::string &str, uint8_t &out) {
  if (str.empty())
    return false;
  char *end;
  int64_t val = strtol(str.c_str(), &end, 10);
  if (end == str.c_str() || *end != '\0' || val < 0 || val > 255) {
    return false;
  }
  out = static_cast<uint8_t>(val);
  return true;
}

// A number the file does not carry would read back as 0 and be stored as such.
static bool read_float(const JsonObject &obj, const char *key, float &out) {
  if (obj[key].isNull() || !obj[key].is<float>()) {
    ESP_LOGE(TAG, "Missing %s", key);
    return false;
  }
  out = obj[key].as<float>();
  return true;
}

// A word the engine does not know reads back as a default, and the next save would write that
// default over what the file says: refuse the rule instead and leave the file alone.
template<typename E>
static bool parse_enum(const JsonObject &obj, const char *key, E (*from_string)(const std::string &),
                       const char *(*to_string)(E), E &out) {
  if (obj[key].isNull()) {
    ESP_LOGE(TAG, "Missing %s", key);
    return false;
  }
  const std::string text = obj[key].as<std::string>();
  out = from_string(text);
  if (text != "none" && text == to_string(out))
    return true;
  ESP_LOGE(TAG, "Unknown %s '%s'", key, text.c_str());
  return false;
}

// Helper function to serialize a vector of uint8_t to cron field string
// Outputs compact notation when possible: *, */N, X-Y, or comma-separated
static std::string serialize_cron_field(const std::vector<uint8_t> &values, uint8_t min_val, uint8_t max_val) {
  if (values.empty()) {
    return "*";
  }

  // Make a sorted copy for analysis
  std::vector<uint8_t> sorted_vals = values;
  std::sort(sorted_vals.begin(), sorted_vals.end());

  // Check if this is a full range (all values present)
  size_t full_range_size = static_cast<size_t>(max_val - min_val + 1);
  if (sorted_vals.size() == full_range_size) {
    bool is_full_range = true;
    for (size_t i = 0; i < sorted_vals.size(); i++) {
      if (sorted_vals[i] != min_val + i) {
        is_full_range = false;
        break;
      }
    }
    if (is_full_range) {
      return "*";
    }
  }

  // Check for step pattern starting from min_val (*/N pattern)
  if (sorted_vals.size() >= 2 && sorted_vals[0] == min_val) {
    uint8_t step = sorted_vals[1] - sorted_vals[0];
    if (step > 1) {
      bool is_step_pattern = true;
      // Verify all values follow the step pattern
      for (size_t i = 1; i < sorted_vals.size(); i++) {
        if (sorted_vals[i] - sorted_vals[i - 1] != step) {
          is_step_pattern = false;
          break;
        }
      }
      // Also verify the pattern covers the expected range
      if (is_step_pattern) {
        // Check that last value is the expected one for this step
        uint8_t expected_last = min_val;
        while (expected_last + step <= max_val) {
          expected_last += step;
        }
        if (sorted_vals.back() == expected_last) {
          return "*/" + std::to_string(step);
        }
      }
    }
  }

  // Check for contiguous range (X-Y pattern)
  if (sorted_vals.size() >= 2) {
    bool is_contiguous = true;
    for (size_t i = 1; i < sorted_vals.size(); i++) {
      if (sorted_vals[i] != sorted_vals[i - 1] + 1) {
        is_contiguous = false;
        break;
      }
    }
    if (is_contiguous) {
      return std::to_string(sorted_vals.front()) + "-" + std::to_string(sorted_vals.back());
    }
  }

  // Single value
  if (sorted_vals.size() == 1) {
    return std::to_string(sorted_vals[0]);
  }

  // Fall back to comma-separated list
  std::string result;
  for (size_t i = 0; i < sorted_vals.size(); i++) {
    if (i > 0) {
      result += ",";
    }
    result += std::to_string(sorted_vals[i]);
  }
  return result;
}

// One member of a cron field: *, */N, X-Y, X-Y/N or N. Anything else, or a value outside the
// field, refuses the member: skipping it would store a rule that runs at other times than written.
static bool parse_cron_part(const std::string &part, uint8_t min_val, uint8_t max_val, std::vector<uint8_t> &result) {
  if (part.empty())
    return false;

  std::string base_part = part;
  uint8_t step = 1;
  const size_t slash_pos = part.find('/');
  if (slash_pos != std::string::npos) {
    base_part = part.substr(0, slash_pos);
    if (!safe_parse_uint8(part.substr(slash_pos + 1), step) || step == 0)
      return false;
  }

  uint8_t range_start = min_val;
  uint8_t range_end = max_val;
  if (base_part != "*") {
    const size_t dash_pos = base_part.find('-');
    if (dash_pos != std::string::npos) {
      if (!safe_parse_uint8(base_part.substr(0, dash_pos), range_start) ||
          !safe_parse_uint8(base_part.substr(dash_pos + 1), range_end))
        return false;
    } else {
      if (!safe_parse_uint8(base_part, range_start))
        return false;
      range_end = range_start;
    }
    if (range_start < min_val || range_end > max_val || range_start > range_end)
      return false;
  }

  for (uint8_t i = range_start; i <= range_end; i += step) {
    result.push_back(i);
    // Prevent overflow when i + step > 255
    if (i > range_end - step && step > 1)
      break;
  }
  return true;
}

// A whole field: comma-separated members, sorted and unique.
static bool deserialize_cron_field(const std::string &field, uint8_t min_val, uint8_t max_val,
                                   std::vector<uint8_t> &result) {
  result.clear();
  size_t start = 0;
  while (start <= field.length()) {
    size_t comma_pos = field.find(',', start);
    if (comma_pos == std::string::npos)
      comma_pos = field.length();
    if (!parse_cron_part(field.substr(start, comma_pos - start), min_val, max_val, result))
      return false;
    start = comma_pos + 1;
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return true;
}

TriggerConfig::TriggerConfig() : source(SourceTrigger::NONE) { memset(&params, 0, sizeof(params)); }

std::string TriggerConfig::cron_string() const {
  return serialize_cron_field(cron_seconds, 0, 60) + " " + serialize_cron_field(cron_minutes, 0, 59) + " " +
         serialize_cron_field(cron_hours, 0, 23) + " " + serialize_cron_field(cron_days_of_month, 1, 31) + " " +
         serialize_cron_field(cron_months, 1, 12) + " " + serialize_cron_field(cron_days_of_week, 1, 7);
}

void TriggerConfig::serialize(JsonObject &obj) const {
  obj["source"] = EnumUtils::source_trigger_to_string(source);

  switch (source) {
    case SourceTrigger::INPUT:
      obj["type"] = EnumUtils::input_trigger_type_to_string(params.input.type);
      obj["object_id"] = binary_sensor_object_id(params.input.input_id);
      break;
    case SourceTrigger::TEMPERATURE:
      obj["type"] = EnumUtils::temperature_trigger_type_to_string(params.temperature.type);
      obj["object_id"] = sensor_object_id(params.temperature.sensor_id);
      if (params.temperature.type == TypesTemperatureTrigger::BELOW ||
          params.temperature.type == TypesTemperatureTrigger::ABOVE) {
        obj["threshold"] = params.temperature.threshold;
      } else if (params.temperature.type == TypesTemperatureTrigger::RANGE) {
        obj["min_threshold"] = params.temperature.min_threshold;
        obj["max_threshold"] = params.temperature.max_threshold;
      }
      break;
    case SourceTrigger::SWITCH:
      obj["type"] = EnumUtils::switch_trigger_type_to_string(params.switch_trigger.type);
      obj["object_id"] = switch_object_id(params.switch_trigger.switch_id);
      break;
    case SourceTrigger::CRON:
      obj["cron"] = cron_string();
      // The engine reads the expression, not the preset; the preset is the editor's own note
      // about which form it presented, so it is kept when there is one and never invented.
      if (cron_preset.has_value())
        obj["cron_preset"] = EnumUtils::cron_preset_to_string(*cron_preset);
      break;
    case SourceTrigger::STARTUP:
    default:
      // Startup carries no extra parameters.
      break;
  }
}

bool TriggerConfig::deserialize(const JsonObject &obj) {
  if (obj["source"].isNull())
    return false;

  if (!parse_enum(obj, "source", EnumUtils::string_to_source_trigger, EnumUtils::source_trigger_to_string, source))
    return false;

  switch (source) {
    case SourceTrigger::INPUT: {
      if (!parse_enum(obj, "type", EnumUtils::string_to_input_trigger_type, EnumUtils::input_trigger_type_to_string,
                      params.input.type))
        return false;
      std::string object_id = obj["object_id"].as<std::string>();
      params.input.input_id = fnv1_hash(object_id);
      break;
    }
    case SourceTrigger::TEMPERATURE: {
      if (!parse_enum(obj, "type", EnumUtils::string_to_temperature_trigger_type,
                      EnumUtils::temperature_trigger_type_to_string, params.temperature.type))
        return false;
      std::string object_id = obj["object_id"].as<std::string>();
      params.temperature.sensor_id = fnv1_hash(object_id);

      if (params.temperature.type == TypesTemperatureTrigger::BELOW ||
          params.temperature.type == TypesTemperatureTrigger::ABOVE) {
        if (!read_float(obj, "threshold", params.temperature.threshold))
          return false;
      } else if (params.temperature.type == TypesTemperatureTrigger::RANGE) {
        if (!read_float(obj, "min_threshold", params.temperature.min_threshold) ||
            !read_float(obj, "max_threshold", params.temperature.max_threshold))
          return false;
      }
      break;
    }
    case SourceTrigger::SWITCH: {
      if (!parse_enum(obj, "type", EnumUtils::string_to_switch_trigger_type, EnumUtils::switch_trigger_type_to_string,
                      params.switch_trigger.type))
        return false;
      std::string object_id = obj["object_id"].as<std::string>();
      params.switch_trigger.switch_id = fnv1_hash(object_id);
      break;
    }
    case SourceTrigger::CRON: {
      if (obj["cron"].isNull())
        return false;

      std::string cron_str = obj["cron"].as<std::string>();

      // Parse space-separated cron string: "seconds minutes hours days_of_month months days_of_week"
      std::vector<std::string> fields;
      size_t start = 0;
      size_t space_pos;
      while ((space_pos = cron_str.find(' ', start)) != std::string::npos) {
        fields.push_back(cron_str.substr(start, space_pos - start));
        start = space_pos + 1;
      }
      // Last field
      if (start < cron_str.length()) {
        fields.push_back(cron_str.substr(start));
      }

      if (fields.size() != 6) {
        ESP_LOGE(TAG, "Invalid cron format, expected 6 fields but got %u", static_cast<unsigned>(fields.size()));
        return false;
      }

      if (!deserialize_cron_field(fields[0], 0, 60, cron_seconds) ||
          !deserialize_cron_field(fields[1], 0, 59, cron_minutes) ||
          !deserialize_cron_field(fields[2], 0, 23, cron_hours) ||
          !deserialize_cron_field(fields[3], 1, 31, cron_days_of_month) ||
          !deserialize_cron_field(fields[4], 1, 12, cron_months) ||
          !deserialize_cron_field(fields[5], 1, 7, cron_days_of_week)) {
        ESP_LOGE(TAG, "Invalid cron '%s': a field cannot be read", cron_str.c_str());
        return false;
      }

      // A field that matched nothing never fires yet comes back as "*", which reads like
      // "always": refuse the rule rather than store that.
      if (cron_seconds.empty() || cron_minutes.empty() || cron_hours.empty() || cron_days_of_month.empty() ||
          cron_months.empty() || cron_days_of_week.empty()) {
        ESP_LOGE(TAG, "Invalid cron '%s': a field matches no value (would never fire)", cron_str.c_str());
        return false;
      }

      if (!obj["cron_preset"].isNull()) {
        CronPreset preset;
        if (!parse_enum(obj, "cron_preset", EnumUtils::string_to_cron_preset, EnumUtils::cron_preset_to_string, preset))
          return false;
        cron_preset = preset;
      }
      break;
    }
    case SourceTrigger::STARTUP: {
      break;
    }
    default:
      break;
  }

  return true;
}

ConditionConfig::ConditionConfig()
    : type(ConditionType::NONE),
      sensor_id(0),
      state(InputConditionState::TRUE),
      temperature_type(TypesTemperatureCondition::NONE),
      threshold(0.0f),
      min_threshold(0.0f),
      max_threshold(0.0f) {}

void ConditionConfig::serialize(JsonObject &obj) const {
  obj["type"] = EnumUtils::condition_type_to_string(type);

  switch (type) {
    case ConditionType::AND:
    case ConditionType::OR:
    case ConditionType::XOR: {
      JsonArray sub_array = obj["conditions"].to<JsonArray>();
      for (const auto &sub : sub_conditions) {
        JsonObject sub_obj = sub_array.add<JsonObject>();
        sub.serialize(sub_obj);
      }
      break;
    }
    case ConditionType::INPUT:
      obj["object_id"] = binary_sensor_object_id(sensor_id);
      obj["state"] = EnumUtils::input_condition_state_to_string(state);
      break;
    case ConditionType::TEMPERATURE:
      obj["object_id"] = sensor_object_id(sensor_id);
      obj["temperature_type"] = EnumUtils::temperature_condition_type_to_string(temperature_type);
      if (temperature_type == TypesTemperatureCondition::BELOW ||
          temperature_type == TypesTemperatureCondition::ABOVE) {
        obj["threshold"] = threshold;
      } else if (temperature_type == TypesTemperatureCondition::RANGE) {
        obj["min_threshold"] = min_threshold;
        obj["max_threshold"] = max_threshold;
      }
      break;
    default:
      break;
  }
}

bool ConditionConfig::deserialize(const JsonObject &obj) {
  if (obj["type"].isNull())
    return false;

  if (!parse_enum(obj, "type", EnumUtils::string_to_condition_type, EnumUtils::condition_type_to_string, type))
    return false;

  switch (type) {
    case ConditionType::AND:
    case ConditionType::OR:
    case ConditionType::XOR: {
      if (!obj["conditions"].isNull()) {
        JsonArray sub_array = obj["conditions"].as<JsonArray>();
        for (const auto &sub_obj : sub_array) {
          ConditionConfig sub_condition;
          if (!sub_condition.deserialize(sub_obj.as<JsonObject>()))
            return false;
          sub_conditions.push_back(sub_condition);
        }
      }
      // Fail closed: a group that lost its members would quietly stop gating anything.
      if (sub_conditions.empty()) {
        ESP_LOGE(TAG, "Condition '%s' has no members", EnumUtils::condition_type_to_string(type));
        return false;
      }
      break;
    }
    case ConditionType::INPUT: {
      std::string object_id = obj["object_id"].as<std::string>();
      sensor_id = fnv1_hash(object_id);
      if (!obj["state"].isNull() && !parse_enum(obj, "state", EnumUtils::string_to_input_condition_state,
                                                EnumUtils::input_condition_state_to_string, state))
        return false;
      break;
    }
    case ConditionType::TEMPERATURE: {
      std::string object_id = obj["object_id"].as<std::string>();
      sensor_id = fnv1_hash(object_id);
      if (!parse_enum(obj, "temperature_type", EnumUtils::string_to_temperature_condition_type,
                      EnumUtils::temperature_condition_type_to_string, temperature_type))
        return false;

      if (temperature_type == TypesTemperatureCondition::BELOW ||
          temperature_type == TypesTemperatureCondition::ABOVE) {
        if (!read_float(obj, "threshold", threshold))
          return false;
      } else if (temperature_type == TypesTemperatureCondition::RANGE) {
        if (!read_float(obj, "min_threshold", min_threshold) || !read_float(obj, "max_threshold", max_threshold))
          return false;
      }
      break;
    }
    default:
      break;
  }

  return true;
}

ActionConfig::ActionConfig() : source(SourceAction::NONE) { memset(&params, 0, sizeof(params)); }

void ActionConfig::serialize(JsonObject &obj) const {
  obj["source"] = EnumUtils::source_action_to_string(source);

  switch (source) {
    case SourceAction::SWITCH:
      obj["type"] = EnumUtils::switch_action_type_to_string(params.switch_action.type);
      obj["object_id"] = switch_object_id(params.switch_action.switch_id);
      // Written where it means something, like the temperature thresholds.
      if (params.switch_action.type == TypeSwitchAction::FOLLOW)
        obj["invert"] = params.switch_action.invert;
      break;
    case SourceAction::DELAY:
      obj["delay_ms"] = params.delay.delay_ms;
      break;
    default:
      break;
  }
}

bool ActionConfig::deserialize(const JsonObject &obj) {
  if (obj["source"].isNull())
    return false;

  if (!parse_enum(obj, "source", EnumUtils::string_to_source_action, EnumUtils::source_action_to_string, source))
    return false;

  switch (source) {
    case SourceAction::SWITCH: {
      if (!parse_enum(obj, "type", EnumUtils::string_to_switch_action_type, EnumUtils::switch_action_type_to_string,
                      params.switch_action.type))
        return false;
      std::string object_id = obj["object_id"].as<std::string>();
      params.switch_action.switch_id = fnv1_hash(object_id);
      params.switch_action.invert = obj["invert"].as<bool>();  // absent reads false
      break;
    }
    case SourceAction::DELAY: {
      // delay_s is what every file written before this stored. Read either, write only
      // delay_ms; both clamp, because either used to reach the scheduler as a wrapped uint32.
      const bool in_ms = !obj["delay_ms"].isNull();
      if (!in_ms && obj["delay_s"].isNull()) {
        ESP_LOGE(TAG, "Missing delay_ms");
        return false;
      }
      const double ms = in_ms ? obj["delay_ms"].as<double>() : obj["delay_s"].as<double>() * 1000;
      const double limit = MAX_DELAY_MS;
      params.delay.delay_ms = static_cast<uint32_t>(ms > 0 ? std::min(ms, limit) : 0.0);
      break;
    }
    default:
      break;
  }

  return true;
}

void AutomationConfig::serialize(JsonObject &obj) const {
  obj["id"] = id;
  obj["name"] = name;
  obj["enabled"] = enabled;
  obj["mode"] = EnumUtils::automation_mode_to_string(mode);

  // Serialize triggers as array (new format)
  JsonArray triggers_array = obj["triggers"].to<JsonArray>();
  for (const auto &trigger : triggers) {
    JsonObject trigger_obj = triggers_array.add<JsonObject>();
    trigger.serialize(trigger_obj);
  }

  // Serialize condition if it exists
  if (condition.is_valid()) {
    JsonObject condition_obj = obj["condition"].to<JsonObject>();
    condition.serialize(condition_obj);
  }

  JsonArray actions_array = obj["actions"].to<JsonArray>();
  for (const auto &action : actions) {
    JsonObject action_obj = actions_array.add<JsonObject>();
    action.serialize(action_obj);
  }

  // Serialize else_actions if they exist
  if (!else_actions.empty()) {
    JsonArray else_actions_array = obj["else_actions"].to<JsonArray>();
    for (const auto &action : else_actions) {
      JsonObject action_obj = else_actions_array.add<JsonObject>();
      action.serialize(action_obj);
    }
  }
}

bool AutomationConfig::deserialize(const JsonObject &obj) {
  if (obj["name"].isNull()) {
    return false;
  }

  id = obj["id"] | 0u;  // Optional: old files won't have it, default to 0 (unassigned)
  name = obj["name"].as<std::string>();
  // Absent reads false in ArduinoJson; a rule is worth writing only if it should run.
  enabled = obj["enabled"].isNull() ? true : obj["enabled"].as<bool>();
  if (!obj["mode"].isNull() &&
      !parse_enum(obj, "mode", EnumUtils::string_to_automation_mode, EnumUtils::automation_mode_to_string, mode))
    return false;

  // Support both old format (single "trigger") and new format ("triggers" array)
  if (!obj["triggers"].isNull()) {
    // New format: array of triggers
    JsonArray triggers_array = obj["triggers"].as<JsonArray>();
    for (const auto &trigger_obj : triggers_array) {
      TriggerConfig trigger;
      if (trigger.deserialize(trigger_obj.as<JsonObject>())) {
        triggers.push_back(trigger);
      } else {
        ESP_LOGE(TAG, "Failed to load a trigger from the 'triggers' array");
        return false;
      }
    }
  } else if (!obj["trigger"].isNull()) {
    // Old format: single trigger - convert to array with one element
    TriggerConfig trigger;
    if (!trigger.deserialize(obj["trigger"].as<JsonObject>())) {
      ESP_LOGE(TAG, "Failed to load the 'trigger'");
      return false;
    }
    triggers.push_back(trigger);
  } else {
    // No trigger or triggers field
    ESP_LOGE(TAG, "Automation config missing 'trigger' or 'triggers' field");
    return false;
  }

  // A condition may be absent, but one that is present must parse: a rule built without the
  // gate it was written with would run its actions unconditionally.
  if (!obj["condition"].isNull()) {
    if (!condition.deserialize(obj["condition"].as<JsonObject>())) {
      ESP_LOGE(TAG, "Failed to load the 'condition' (present but malformed)");
      return false;
    }
  }

  if (!obj["actions"].isNull()) {
    JsonArray actions_array = obj["actions"].as<JsonArray>();
    for (const auto &action_obj : actions_array) {
      ActionConfig action;
      if (action.deserialize(action_obj.as<JsonObject>())) {
        actions.push_back(action);
      } else {
        ESP_LOGE(TAG, "Failed to load an action");
        return false;
      }
    }
  }

  // Deserialize else_actions if they exist (optional)
  if (!obj["else_actions"].isNull()) {
    JsonArray else_actions_array = obj["else_actions"].as<JsonArray>();
    for (const auto &action_obj : else_actions_array) {
      ActionConfig action;
      if (action.deserialize(action_obj.as<JsonObject>())) {
        else_actions.push_back(action);
      } else {
        ESP_LOGE(TAG, "Failed to load an else_action");
        return false;
      }
    }
  }

  return true;
}

void AutomationConfigStorage::add_config(const AutomationConfig &config) { configs_.push_back(config); }

void AutomationConfigStorage::update_config(uint8_t index, AutomationConfig *config) {
  if (index < configs_.size()) {
    configs_[index] = *config;
  }
}

bool AutomationConfigStorage::remove_config(uint8_t index) {
  if (index < configs_.size()) {
    configs_.erase(configs_.begin() + index);
    return true;
  }
  return false;
}

AutomationConfig *AutomationConfigStorage::get_config(uint8_t index) {
  if (index < configs_.size()) {
    return &(configs_[index]);
  }
  return nullptr;
}

void AutomationConfigStorage::sort_by_id() {
  std::sort(configs_.begin(), configs_.end(),
            [](const AutomationConfig &a, const AutomationConfig &b) { return a.id < b.id; });
}

}  // namespace esphome::automations
