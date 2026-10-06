// The enum strings are the stored format, not a logging detail: a thermostat written by one
// build has to be readable by the next.
#include "common.h"
#include "esphome/components/climate_hub/enums.h"

namespace esphome::climate_hub::testing {

TEST(Enums, ControlKindRoundTrips) {
  for (ControlKind kind : {ControlKind::PID, ControlKind::BANG_BANG}) {
    ControlKind parsed;
    ASSERT_TRUE(enums::control_kind_from_string(enums::control_kind_to_string(kind), &parsed));
    EXPECT_EQ(kind, parsed);
  }
}

TEST(Enums, ControlKindRejectsUnknown) {
  ControlKind parsed = ControlKind::BANG_BANG;
  EXPECT_FALSE(enums::control_kind_from_string("thermostat", &parsed));
  EXPECT_EQ(ControlKind::BANG_BANG, parsed);
}

TEST(Enums, ModeRoundTrips) {
  for (HubMode mode : {HubMode::OFF, HubMode::HEAT, HubMode::COOL, HubMode::HEAT_COOL}) {
    HubMode parsed;
    ASSERT_TRUE(enums::mode_from_string(enums::mode_to_string(mode), &parsed));
    EXPECT_EQ(mode, parsed);
  }
}

TEST(Enums, ModeRejectsUnknown) {
  HubMode parsed = HubMode::HEAT;
  EXPECT_FALSE(enums::mode_from_string("dry", &parsed));
  EXPECT_EQ(HubMode::HEAT, parsed);
}

TEST(Enums, ActionAndFaultNamesAreStable) {
  EXPECT_STREQ("off", enums::action_to_string(HubAction::OFF));
  EXPECT_STREQ("idle", enums::action_to_string(HubAction::IDLE));
  EXPECT_STREQ("heating", enums::action_to_string(HubAction::HEATING));
  EXPECT_STREQ("cooling", enums::action_to_string(HubAction::COOLING));

  EXPECT_STREQ("none", enums::fault_to_string(HubFault::NONE));
  EXPECT_STREQ("sensor_stale", enums::fault_to_string(HubFault::SENSOR_STALE));
  EXPECT_STREQ("overtemp", enums::fault_to_string(HubFault::OVERTEMP));
  EXPECT_STREQ("relay_contested", enums::fault_to_string(HubFault::RELAY_CONTESTED));
}

// The calibration's words: the API's, which the dashboard shows.
TEST(Enums, CalibrationNamesAreStable) {
  EXPECT_STREQ("running", enums::autotune_state_to_string(AutotuneState::RUNNING));
  EXPECT_STREQ("succeeded", enums::autotune_state_to_string(AutotuneState::SUCCEEDED));
  EXPECT_STREQ("failed", enums::autotune_state_to_string(AutotuneState::FAILED));

  const std::pair<AutotuneEnd, const char *> ends[] = {
      {AutotuneEnd::NONE, ""},
      {AutotuneEnd::CANCELLED, "cancelled"},
      {AutotuneEnd::TARGET_CHANGED, "target_changed"},
      {AutotuneEnd::MODE_CHANGED, "mode_changed"},
      {AutotuneEnd::SAVED, "saved"},
      {AutotuneEnd::STOPPED, "stopped"},
      {AutotuneEnd::TAKEN_OVER, "taken_over"},
      {AutotuneEnd::SENSOR_STALE, "sensor_stale"},
      {AutotuneEnd::OVERTEMP, "overtemp"},
      {AutotuneEnd::RELAY_CONTESTED, "relay_contested"},
      {AutotuneEnd::TIMEOUT, "timeout"},
      {AutotuneEnd::NO_SWITCH, "no_switch"},
  };
  for (const auto &end : ends)
    EXPECT_STREQ(end.second, enums::autotune_end_to_string(end.first));

  // A fault ends a run under its own name.
  for (HubFault fault : {HubFault::SENSOR_STALE, HubFault::OVERTEMP, HubFault::RELAY_CONTESTED})
    EXPECT_STREQ(enums::fault_to_string(fault), enums::autotune_end_to_string(enums::autotune_end_of(fault)));
  EXPECT_EQ(AutotuneEnd::NONE, enums::autotune_end_of(HubFault::NONE));
}

TEST(Enums, CalibrationRulesAndDirectionsRoundTrip) {
  for (AutotuneRule rule : {AutotuneRule::ZN_PI, AutotuneRule::ZN_PID, AutotuneRule::PESSEN,
                            AutotuneRule::SOME_OVERSHOOT, AutotuneRule::NO_OVERSHOOT}) {
    AutotuneRule parsed;
    ASSERT_TRUE(enums::autotune_rule_from_string(enums::autotune_rule_to_string(rule), &parsed));
    EXPECT_EQ(rule, parsed);
  }
  AutotuneRule rule = AutotuneRule::PESSEN;
  EXPECT_FALSE(enums::autotune_rule_from_string("ziegler", &rule));
  EXPECT_EQ(AutotuneRule::PESSEN, rule);

  for (AutotuneDirection direction : {AutotuneDirection::HEAT, AutotuneDirection::COOL}) {
    AutotuneDirection parsed;
    ASSERT_TRUE(enums::autotune_direction_from_string(enums::autotune_direction_to_string(direction), &parsed));
    EXPECT_EQ(direction, parsed);
  }
  AutotuneDirection direction = AutotuneDirection::COOL;
  EXPECT_FALSE(enums::autotune_direction_from_string("heat_cool", &direction));
  EXPECT_EQ(AutotuneDirection::COOL, direction);
}

}  // namespace esphome::climate_hub::testing
