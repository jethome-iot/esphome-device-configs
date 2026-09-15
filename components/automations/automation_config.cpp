#include "automation_config.h"
#include <algorithm>
#include "esphome/core/helpers.h"
#include "entity_lookup.h"
#include "esphome/core/log.h"

namespace esphome::automations {

static const char *const TAG = "automations";

// Past this a delay in seconds no longer fits the scheduler's uint32 of milliseconds.
static constexpr uint32_t MAX_DELAY_S = UINT32_MAX / 1000;

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

// Helper to parse a single cron part (handles *, */N, X-Y, X-Y/N, N)
static void parse_cron_part(const std::string &part, uint8_t min_val, uint8_t max_val, std::vector<uint8_t> &result) {
  if (part.empty())
    return;

  // Check for step notation (contains '/')
  std::string base_part = part;
  uint8_t step = 1;
  size_t slash_pos = part.find('/');
  if (slash_pos != std::string::npos) {
    base_part = part.substr(0, slash_pos);
    std::string step_str = part.substr(slash_pos + 1);
    if (!safe_parse_uint8(step_str, step) || step == 0) {
      return;  // Invalid step, skip this part
    }
  }

  uint8_t range_start = min_val;
  uint8_t range_end = max_val;

  if (base_part == "*") {
    // Full range with optional step: * or */N
    range_start = min_val;
    range_end = max_val;
  } else {
    // Check for range notation (contains '-')
    size_t dash_pos = base_part.find('-');
    if (dash_pos != std::string::npos) {
      // Range: X-Y
      std::string start_str = base_part.substr(0, dash_pos);
      std::string end_str = base_part.substr(dash_pos + 1);
      if (!safe_parse_uint8(start_str, range_start) || !safe_parse_uint8(end_str, range_end)) {
        return;  // Invalid range, skip this part
      }
      // Clamp to valid range
      if (range_start < min_val)
        range_start = min_val;
      if (range_end > max_val)
        range_end = max_val;
      if (range_start > range_end)
        return;  // Invalid range
    } else {
      // Single value
      uint8_t val;
      if (!safe_parse_uint8(base_part, val)) {
        return;  // Invalid value, skip this part
      }
      if (val >= min_val && val <= max_val) {
        result.push_back(val);
      }
      return;
    }
  }

  // Generate values in range with step
  for (uint8_t i = range_start; i <= range_end; i += step) {
    result.push_back(i);
    // Prevent overflow when i + step > 255
    if (i > range_end - step && step > 1)
      break;
  }
}

// Helper function to deserialize a cron field string to vector
// Supports: *, */N, X-Y, X-Y/N, N, and comma-separated combinations
static std::vector<uint8_t> deserialize_cron_field(const std::string &field, uint8_t min_val, uint8_t max_val) {
  std::vector<uint8_t> result;

  if (field.empty()) {
    return result;
  }

  // Parse comma-separated parts
  size_t start = 0;
  size_t comma_pos;
  while ((comma_pos = field.find(',', start)) != std::string::npos) {
    std::string part = field.substr(start, comma_pos - start);
    parse_cron_part(part, min_val, max_val, result);
    start = comma_pos + 1;
  }
  // Last part
  if (start < field.length()) {
    std::string part = field.substr(start);
    parse_cron_part(part, min_val, max_val, result);
  }

  // Sort and remove duplicates
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());

  return result;
}

TriggerConfig::TriggerConfig() : source(SourceTrigger::NONE) { memset(&params, 0, sizeof(params)); }

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
    case SourceTrigger::CRON: {
      // Serialize as space-separated cron string: "seconds minutes hours days_of_month months days_of_week"
      std::string cron_str;
      cron_str += serialize_cron_field(cron_seconds, 0, 60);
      cron_str += " ";
      cron_str += serialize_cron_field(cron_minutes, 0, 59);
      cron_str += " ";
      cron_str += serialize_cron_field(cron_hours, 0, 23);
      cron_str += " ";
      cron_str += serialize_cron_field(cron_days_of_month, 1, 31);
      cron_str += " ";
      cron_str += serialize_cron_field(cron_months, 1, 12);
      cron_str += " ";
      cron_str += serialize_cron_field(cron_days_of_week, 1, 7);
      obj["cron"] = cron_str;
      obj["cron_preset"] = EnumUtils::cron_preset_to_string(cron_preset);
      break;
    }
    case SourceTrigger::STARTUP:
    default:
      // Startup carries no extra parameters.
      break;
  }
}

bool TriggerConfig::deserialize(const JsonObject &obj) {
  if (obj["source"].isNull())
    return false;

  source = EnumUtils::string_to_source_trigger(obj["source"].as<std::string>());

  switch (source) {
    case SourceTrigger::INPUT: {
      params.input.type = EnumUtils::string_to_input_trigger_type(obj["type"].as<std::string>());
      std::string object_id = obj["object_id"].as<std::string>();
      params.input.input_id = fnv1_hash(object_id);
      break;
    }
    case SourceTrigger::TEMPERATURE: {
      params.temperature.type = EnumUtils::string_to_temperature_trigger_type(obj["type"].as<std::string>());
      std::string object_id = obj["object_id"].as<std::string>();
      params.temperature.sensor_id = fnv1_hash(object_id);

      if (params.temperature.type == TypesTemperatureTrigger::BELOW ||
          params.temperature.type == TypesTemperatureTrigger::ABOVE) {
        params.temperature.threshold = obj["threshold"].as<float>();
      } else if (params.temperature.type == TypesTemperatureTrigger::RANGE) {
        params.temperature.min_threshold = obj["min_threshold"].as<float>();
        params.temperature.max_threshold = obj["max_threshold"].as<float>();
      }
      break;
    }
    case SourceTrigger::SWITCH: {
      params.switch_trigger.type = EnumUtils::string_to_switch_trigger_type(obj["type"].as<std::string>());
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

      cron_seconds = deserialize_cron_field(fields[0], 0, 60);
      cron_minutes = deserialize_cron_field(fields[1], 0, 59);
      cron_hours = deserialize_cron_field(fields[2], 0, 23);
      cron_days_of_month = deserialize_cron_field(fields[3], 1, 31);
      cron_months = deserialize_cron_field(fields[4], 1, 12);
      cron_days_of_week = deserialize_cron_field(fields[5], 1, 7);

      // Reject a cron whose any field matched no in-range value. Such a field
      // leaves its bitset all-zero (the trigger never fires) yet re-serialises as
      // "*" (serialize_cron_field of an empty vector), so accepting it would
      // persist a rule that silently never runs and comes back looking like it
      // runs constantly. Every valid field yields at least one value ("*" fills
      // the whole range), so an empty vector here always means bad input.
      if (cron_seconds.empty() || cron_minutes.empty() || cron_hours.empty() || cron_days_of_month.empty() ||
          cron_months.empty() || cron_days_of_week.empty()) {
        ESP_LOGE(TAG, "Invalid cron '%s': a field matches no value (would never fire)", cron_str.c_str());
        return false;
      }

      // Load preset if available, default to Daily for old configs
      if (!obj["cron_preset"].isNull()) {
        cron_preset = EnumUtils::string_to_cron_preset(obj["cron_preset"].as<std::string>());
      } else {
        cron_preset = CronPreset::DAILY;  // Default for old configs
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

  type = EnumUtils::string_to_condition_type(obj["type"].as<std::string>());

  switch (type) {
    case ConditionType::AND:
    case ConditionType::OR:
    case ConditionType::XOR: {
      if (!obj["conditions"].isNull()) {
        JsonArray sub_array = obj["conditions"].as<JsonArray>();
        for (const auto &sub_obj : sub_array) {
          ConditionConfig sub_condition;
          if (sub_condition.deserialize(sub_obj.as<JsonObject>())) {
            sub_conditions.push_back(sub_condition);
          }
        }
      }
      break;
    }
    case ConditionType::INPUT: {
      std::string object_id = obj["object_id"].as<std::string>();
      sensor_id = fnv1_hash(object_id);
      if (!obj["state"].isNull()) {
        state = EnumUtils::string_to_input_condition_state(obj["state"].as<std::string>());
      }
      break;
    }
    case ConditionType::TEMPERATURE: {
      std::string object_id = obj["object_id"].as<std::string>();
      sensor_id = fnv1_hash(object_id);
      temperature_type = EnumUtils::string_to_temperature_condition_type(obj["temperature_type"].as<std::string>());

      if (temperature_type == TypesTemperatureCondition::BELOW ||
          temperature_type == TypesTemperatureCondition::ABOVE) {
        threshold = obj["threshold"].as<float>();
      } else if (temperature_type == TypesTemperatureCondition::RANGE) {
        min_threshold = obj["min_threshold"].as<float>();
        max_threshold = obj["max_threshold"].as<float>();
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

  source = EnumUtils::string_to_source_action(obj["source"].as<std::string>());

  switch (source) {
    case SourceAction::SWITCH: {
      params.switch_action.type = EnumUtils::string_to_switch_action_type(obj["type"].as<std::string>());
      std::string object_id = obj["object_id"].as<std::string>();
      params.switch_action.switch_id = fnv1_hash(object_id);
      params.switch_action.invert = obj["invert"].as<bool>();  // absent reads false
      break;
    }
    case SourceAction::DELAY:
      // delay_s is what every file written before this stored. Read either, write
      // only delay_ms; the seconds are clamped because they used to be multiplied
      // into a uint32 that silently wrapped past ~49.7 days.
      if (obj["delay_ms"].is<uint32_t>()) {
        params.delay.delay_ms = obj["delay_ms"].as<uint32_t>();
      } else {
        uint32_t seconds = obj["delay_s"].as<uint32_t>();
        params.delay.delay_ms = seconds > MAX_DELAY_S ? UINT32_MAX : seconds * 1000;
      }
      break;
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
  enabled = obj["enabled"].as<bool>();
  mode = obj["mode"].isNull() ? AutomationMode::SINGLE
                              : EnumUtils::string_to_automation_mode(obj["mode"].as<std::string>());

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

  // A condition is OPTIONAL (may be absent), but a condition that IS present must
  // parse. deserialize only fails on a condition object with no "type", which is
  // malformed — accepting it silently would drop the gate and, worse, discard
  // else_actions (the factory attaches them only inside the IfAction branch).
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
