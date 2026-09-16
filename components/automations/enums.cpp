#include "enums.h"
#include <string>

namespace esphome::automations {
namespace EnumUtils {

const char *source_trigger_to_string(SourceTrigger source) {
  switch (source) {
    case SourceTrigger::INPUT:
      return "input";
    case SourceTrigger::TEMPERATURE:
      return "temperature";
    case SourceTrigger::CRON:
      return "cron";
    case SourceTrigger::STARTUP:
      return "startup";
    case SourceTrigger::SWITCH:
      return "switch";
    default:
      return "none";
  }
}

SourceTrigger string_to_source_trigger(const std::string &str) {
  if (str == "input")
    return SourceTrigger::INPUT;
  if (str == "temperature")
    return SourceTrigger::TEMPERATURE;
  if (str == "cron")
    return SourceTrigger::CRON;
  if (str == "startup")
    return SourceTrigger::STARTUP;
  if (str == "switch")
    return SourceTrigger::SWITCH;
  return SourceTrigger::NONE;
}

const char *switch_trigger_type_to_string(TypesSwitchTrigger type) {
  switch (type) {
    case TypesSwitchTrigger::TURN_ON:
      return "turn_on";
    case TypesSwitchTrigger::TURN_OFF:
      return "turn_off";
    case TypesSwitchTrigger::STATE_CHANGE:
      return "state_change";
    default:
      return "none";
  }
}

TypesSwitchTrigger string_to_switch_trigger_type(const std::string &str) {
  if (str == "turn_on")
    return TypesSwitchTrigger::TURN_ON;
  if (str == "turn_off")
    return TypesSwitchTrigger::TURN_OFF;
  if (str == "state_change")
    return TypesSwitchTrigger::STATE_CHANGE;
  return TypesSwitchTrigger::NONE;
}

const char *source_action_to_string(SourceAction source) {
  switch (source) {
    case SourceAction::DELAY:
      return "delay";
    case SourceAction::SWITCH:
      return "switch";
    default:
      return "none";
  }
}

SourceAction string_to_source_action(const std::string &str) {
  if (str == "delay")
    return SourceAction::DELAY;
  if (str == "switch")
    return SourceAction::SWITCH;
  return SourceAction::NONE;
}

// TypesInputTrigger
const char *input_trigger_type_to_string(TypesInputTrigger type) {
  switch (type) {
    case TypesInputTrigger::PRESS:
      return "press";
    case TypesInputTrigger::RELEASE:
      return "release";
    case TypesInputTrigger::CLICK:
      return "click";
    case TypesInputTrigger::STATE_CHANGE:
      return "state_change";
    default:
      return "none";
  }
}

TypesInputTrigger string_to_input_trigger_type(const std::string &str) {
  if (str == "press")
    return TypesInputTrigger::PRESS;
  if (str == "release")
    return TypesInputTrigger::RELEASE;
  if (str == "click")
    return TypesInputTrigger::CLICK;
  if (str == "state_change")
    return TypesInputTrigger::STATE_CHANGE;
  return TypesInputTrigger::NONE;
}

// TypesTemperatureTrigger
const char *temperature_trigger_type_to_string(TypesTemperatureTrigger type) {
  switch (type) {
    case TypesTemperatureTrigger::BELOW:
      return "below";
    case TypesTemperatureTrigger::ABOVE:
      return "above";
    case TypesTemperatureTrigger::RANGE:
      return "range";
    default:
      return "none";
  }
}

TypesTemperatureTrigger string_to_temperature_trigger_type(const std::string &str) {
  if (str == "below")
    return TypesTemperatureTrigger::BELOW;
  if (str == "above")
    return TypesTemperatureTrigger::ABOVE;
  if (str == "range")
    return TypesTemperatureTrigger::RANGE;
  return TypesTemperatureTrigger::NONE;
}

// TypeSwitchAction
const char *switch_action_type_to_string(TypeSwitchAction type) {
  switch (type) {
    case TypeSwitchAction::TURN_ON:
      return "turn_on";
    case TypeSwitchAction::TURN_OFF:
      return "turn_off";
    case TypeSwitchAction::TOGGLE:
      return "toggle";
    case TypeSwitchAction::FOLLOW:
      return "follow";
    default:
      return "none";
  }
}

TypeSwitchAction string_to_switch_action_type(const std::string &str) {
  if (str == "turn_on")
    return TypeSwitchAction::TURN_ON;
  if (str == "turn_off")
    return TypeSwitchAction::TURN_OFF;
  if (str == "toggle")
    return TypeSwitchAction::TOGGLE;
  if (str == "follow")
    return TypeSwitchAction::FOLLOW;
  return TypeSwitchAction::NONE;
}

// ConditionType
const char *condition_type_to_string(ConditionType type) {
  switch (type) {
    case ConditionType::AND:
      return "and";
    case ConditionType::OR:
      return "or";
    case ConditionType::XOR:
      return "xor";
    case ConditionType::INPUT:
      return "input";
    case ConditionType::TEMPERATURE:
      return "temperature";
    default:
      return "none";
  }
}

ConditionType string_to_condition_type(const std::string &str) {
  if (str == "and")
    return ConditionType::AND;
  if (str == "or")
    return ConditionType::OR;
  if (str == "xor")
    return ConditionType::XOR;
  if (str == "input")
    return ConditionType::INPUT;
  if (str == "temperature")
    return ConditionType::TEMPERATURE;
  return ConditionType::NONE;
}

// InputConditionState
const char *input_condition_state_to_string(InputConditionState state) {
  switch (state) {
    case InputConditionState::TRUE:
      return "true";
    case InputConditionState::FALSE:
    default:
      return "false";
  }
}

InputConditionState string_to_input_condition_state(const std::string &str) {
  if (str == "true")
    return InputConditionState::TRUE;
  return InputConditionState::FALSE;
}

// TypesTemperatureCondition
const char *temperature_condition_type_to_string(TypesTemperatureCondition type) {
  switch (type) {
    case TypesTemperatureCondition::BELOW:
      return "below";
    case TypesTemperatureCondition::ABOVE:
      return "above";
    case TypesTemperatureCondition::RANGE:
      return "range";
    default:
      return "none";
  }
}

TypesTemperatureCondition string_to_temperature_condition_type(const std::string &str) {
  if (str == "below")
    return TypesTemperatureCondition::BELOW;
  if (str == "above")
    return TypesTemperatureCondition::ABOVE;
  if (str == "range")
    return TypesTemperatureCondition::RANGE;
  return TypesTemperatureCondition::NONE;
}

// CronPreset
const char *cron_preset_to_string(CronPreset preset) {
  switch (preset) {
    case CronPreset::DAILY:
      return "daily";
    case CronPreset::HOURLY:
      return "hourly";
    case CronPreset::EVERY_N_MINUTES:
      return "every_n_minutes";
    case CronPreset::WEEKLY:
      return "weekly";
    case CronPreset::MONTHLY:
      return "monthly";
    case CronPreset::CUSTOM:
      return "custom";
    default:
      return "daily";
  }
}

CronPreset string_to_cron_preset(const std::string &str) {
  if (str == "daily")
    return CronPreset::DAILY;
  if (str == "hourly")
    return CronPreset::HOURLY;
  if (str == "every_n_minutes")
    return CronPreset::EVERY_N_MINUTES;
  if (str == "weekly")
    return CronPreset::WEEKLY;
  if (str == "monthly")
    return CronPreset::MONTHLY;
  if (str == "custom")
    return CronPreset::CUSTOM;
  return CronPreset::DAILY;
}

const char *automation_mode_to_string(AutomationMode mode) {
  switch (mode) {
    case AutomationMode::SINGLE:
      return "single";
    case AutomationMode::RESTART:
      return "restart";
    case AutomationMode::PARALLEL:
      return "parallel";
    default:
      return "";
  }
}

AutomationMode string_to_automation_mode(const std::string &str) {
  if (str == "single")
    return AutomationMode::SINGLE;
  if (str == "restart")
    return AutomationMode::RESTART;
  if (str == "parallel")
    return AutomationMode::PARALLEL;
  return AutomationMode::SINGLE;
}

}  // namespace EnumUtils

}  // namespace esphome::automations
