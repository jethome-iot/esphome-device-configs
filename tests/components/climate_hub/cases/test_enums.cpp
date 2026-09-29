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
  EXPECT_STREQ("sensor_missing", enums::fault_to_string(HubFault::SENSOR_MISSING));
  EXPECT_STREQ("sensor_stale", enums::fault_to_string(HubFault::SENSOR_STALE));
  EXPECT_STREQ("relay_missing", enums::fault_to_string(HubFault::RELAY_MISSING));
  EXPECT_STREQ("overtemp", enums::fault_to_string(HubFault::OVERTEMP));
}

}  // namespace esphome::climate_hub::testing
