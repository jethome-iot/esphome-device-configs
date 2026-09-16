#include "common.h"

namespace esphome::automations::testing {

using namespace EnumUtils;

TEST(Enums, SourceTriggerRoundTrip) {
  for (auto v : {SourceTrigger::INPUT, SourceTrigger::TEMPERATURE, SourceTrigger::CRON, SourceTrigger::STARTUP,
                 SourceTrigger::SWITCH})
    EXPECT_EQ(string_to_source_trigger(source_trigger_to_string(v)), v);
  EXPECT_EQ(string_to_source_trigger("inupt"), SourceTrigger::NONE);
}

TEST(Enums, InputTriggerTypeRoundTrip) {
  for (auto v : {TypesInputTrigger::PRESS, TypesInputTrigger::RELEASE, TypesInputTrigger::CLICK,
                 TypesInputTrigger::STATE_CHANGE})
    EXPECT_EQ(string_to_input_trigger_type(input_trigger_type_to_string(v)), v);
  EXPECT_EQ(string_to_input_trigger_type("pres"), TypesInputTrigger::NONE);
}

TEST(Enums, TemperatureTriggerTypeRoundTrip) {
  for (auto v : {TypesTemperatureTrigger::BELOW, TypesTemperatureTrigger::ABOVE, TypesTemperatureTrigger::RANGE})
    EXPECT_EQ(string_to_temperature_trigger_type(temperature_trigger_type_to_string(v)), v);
  EXPECT_EQ(string_to_temperature_trigger_type("between"), TypesTemperatureTrigger::NONE);
}

TEST(Enums, SwitchTriggerTypeRoundTrip) {
  for (auto v : {TypesSwitchTrigger::TURN_ON, TypesSwitchTrigger::TURN_OFF, TypesSwitchTrigger::STATE_CHANGE})
    EXPECT_EQ(string_to_switch_trigger_type(switch_trigger_type_to_string(v)), v);
  EXPECT_EQ(string_to_switch_trigger_type("on"), TypesSwitchTrigger::NONE);
}

TEST(Enums, SourceActionRoundTrip) {
  for (auto v : {SourceAction::DELAY, SourceAction::SWITCH})
    EXPECT_EQ(string_to_source_action(source_action_to_string(v)), v);
  EXPECT_EQ(string_to_source_action("relay"), SourceAction::NONE);
}

TEST(Enums, SwitchActionTypeRoundTrip) {
  for (auto v :
       {TypeSwitchAction::TURN_ON, TypeSwitchAction::TURN_OFF, TypeSwitchAction::TOGGLE, TypeSwitchAction::FOLLOW})
    EXPECT_EQ(string_to_switch_action_type(switch_action_type_to_string(v)), v);
  EXPECT_EQ(string_to_switch_action_type("tugle"), TypeSwitchAction::NONE);
}

TEST(Enums, ConditionTypeRoundTrip) {
  for (auto v :
       {ConditionType::AND, ConditionType::OR, ConditionType::XOR, ConditionType::INPUT, ConditionType::TEMPERATURE})
    EXPECT_EQ(string_to_condition_type(condition_type_to_string(v)), v);
  EXPECT_EQ(string_to_condition_type("nand"), ConditionType::NONE);
}

TEST(Enums, TemperatureConditionTypeRoundTrip) {
  for (auto v : {TypesTemperatureCondition::BELOW, TypesTemperatureCondition::ABOVE, TypesTemperatureCondition::RANGE})
    EXPECT_EQ(string_to_temperature_condition_type(temperature_condition_type_to_string(v)), v);
  EXPECT_EQ(string_to_temperature_condition_type("tempratur"), TypesTemperatureCondition::NONE);
}

TEST(Enums, InputConditionStateRoundTrip) {
  for (auto v : {InputConditionState::FALSE, InputConditionState::TRUE})
    EXPECT_EQ(string_to_input_condition_state(input_condition_state_to_string(v)), v);
}

TEST(Enums, CronPresetRoundTrip) {
  for (auto v : {CronPreset::DAILY, CronPreset::HOURLY, CronPreset::EVERY_N_MINUTES, CronPreset::WEEKLY,
                 CronPreset::MONTHLY, CronPreset::CUSTOM})
    EXPECT_EQ(string_to_cron_preset(cron_preset_to_string(v)), v);
}

TEST(Enums, AutomationModeRoundTrip) {
  for (auto v : {AutomationMode::SINGLE, AutomationMode::RESTART, AutomationMode::PARALLEL})
    EXPECT_EQ(string_to_automation_mode(automation_mode_to_string(v)), v);
}

}  // namespace esphome::automations::testing
