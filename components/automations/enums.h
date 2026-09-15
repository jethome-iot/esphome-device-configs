#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>

namespace esphome::automations {

enum class AutomationMode : uint8_t { SINGLE = 0, RESTART = 1, PARALLEL = 2 };

enum class SourceTrigger : uint8_t { NONE = 0, INPUT, TEMPERATURE, CRON, STARTUP, SWITCH, MAX_TRIGGER_TYPES };

enum class TypesInputTrigger : uint8_t {
  NONE,
  PRESS,
  RELEASE,
  CLICK,
  STATE_CHANGE,
};

enum class TypesTemperatureTrigger : uint8_t {
  NONE,
  BELOW,
  ABOVE,
  RANGE,
};

enum class TypesSwitchTrigger : uint8_t {
  NONE,
  TURN_ON,
  TURN_OFF,
  STATE_CHANGE,
};

enum class SourceAction : uint8_t { NONE = 0, DELAY, SWITCH, MAX_ACTION_TYPES };

enum class TypeSwitchAction : uint8_t {
  NONE,
  TURN_ON,
  TURN_OFF,
  TOGGLE,
  FOLLOW,
};

enum class ConditionType : uint8_t { NONE = 0, AND, OR, XOR, INPUT, TEMPERATURE, MAX_CONDITION_TYPES };

enum class InputConditionState : uint8_t {
  FALSE = 0,
  TRUE = 1,
};

enum class TypesTemperatureCondition : uint8_t {
  NONE = 0,
  BELOW,
  ABOVE,
  RANGE,
};

enum class CronPreset : uint8_t { DAILY = 0, HOURLY, EVERY_N_MINUTES, WEEKLY, MONTHLY, CUSTOM };

constexpr size_t MAX_TRIGGER_TYPES = static_cast<size_t>(SourceTrigger::MAX_TRIGGER_TYPES);
constexpr size_t MAX_ACTION_TYPES = static_cast<size_t>(SourceAction::MAX_ACTION_TYPES);
constexpr size_t MAX_CONDITION_TYPES = static_cast<size_t>(ConditionType::MAX_CONDITION_TYPES);

namespace EnumUtils {
// SourceTrigger
const char *source_trigger_to_string(SourceTrigger source);
SourceTrigger string_to_source_trigger(const std::string &str);

// TypesInputTrigger
const char *input_trigger_type_to_string(TypesInputTrigger type);
TypesInputTrigger string_to_input_trigger_type(const std::string &str);

// TypesTemperatureTrigger
const char *temperature_trigger_type_to_string(TypesTemperatureTrigger type);
TypesTemperatureTrigger string_to_temperature_trigger_type(const std::string &str);

// TypesSwitchTrigger
const char *switch_trigger_type_to_string(TypesSwitchTrigger type);
TypesSwitchTrigger string_to_switch_trigger_type(const std::string &str);

// SourceAction
const char *source_action_to_string(SourceAction source);
SourceAction string_to_source_action(const std::string &str);

// TypeSwitchAction
const char *switch_action_type_to_string(TypeSwitchAction type);
TypeSwitchAction string_to_switch_action_type(const std::string &str);

// ConditionType
const char *condition_type_to_string(ConditionType type);
ConditionType string_to_condition_type(const std::string &str);

// InputConditionState
const char *input_condition_state_to_string(InputConditionState state);
InputConditionState string_to_input_condition_state(const std::string &str);

// TypesTemperatureCondition
const char *temperature_condition_type_to_string(TypesTemperatureCondition type);
TypesTemperatureCondition string_to_temperature_condition_type(const std::string &str);

// CronPreset
const char *cron_preset_to_string(CronPreset preset);
CronPreset string_to_cron_preset(const std::string &str);

// AutomationMode
const char *automation_mode_to_string(AutomationMode mode);
AutomationMode string_to_automation_mode(const std::string &str);

constexpr size_t trigger_to_index(SourceTrigger source) { return static_cast<size_t>(source); }

constexpr size_t action_to_index(SourceAction source) { return static_cast<size_t>(source); }

constexpr size_t condition_to_index(ConditionType type) { return static_cast<size_t>(type); }
}  // namespace EnumUtils

}  // namespace esphome::automations
