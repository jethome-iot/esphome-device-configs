// The two control laws, with dt injected. PidCore is term for term ESPHome's pid controller,
// so the numbers here also pin that equivalence down.
#include "common.h"
#include "esphome/components/climate_hub/hysteresis_core.h"
#include "esphome/components/climate_hub/pid_core.h"

namespace esphome::climate_hub::testing {

TEST(PidCore, ProportionalOnly) {
  PidCore pid;
  pid.set_gains(0.5f, 0.f, 0.f);
  EXPECT_FLOAT_EQ(1.f, pid.update(22.f, 20.f, 1.f));
  EXPECT_FLOAT_EQ(2.f, pid.error());
}

TEST(PidCore, IntegralAccumulatesAndIsClamped) {
  PidCore pid;
  pid.set_gains(0.f, 0.1f, 0.f);
  pid.set_integral_limits(-1.f, 0.5f);

  pid.update(22.f, 20.f, 1.f);
  EXPECT_FLOAT_EQ(0.2f, pid.integral_term());
  pid.update(22.f, 20.f, 1.f);
  EXPECT_FLOAT_EQ(0.4f, pid.integral_term());
  pid.update(22.f, 20.f, 1.f);
  EXPECT_FLOAT_EQ(0.5f, pid.integral_term()) << "clamped at max_integral";
}

TEST(PidCore, NegativeErrorDrivesTheOutputNegative) {
  PidCore pid;
  pid.set_gains(0.5f, 0.f, 0.f);
  EXPECT_LT(pid.update(20.f, 24.f, 1.f), 0.f);
}

TEST(PidCore, DeadbandShallowsTheProportionalTerm) {
  PidCore pid;
  pid.set_gains(1.f, 0.f, 0.f);
  pid.set_deadband(-1.f, 1.f, 0.1f, 0.f, 0.f, 1);

  // 0.5 degrees below the setpoint: inside the deadband, so kp is scaled by 0.1.
  float inside = pid.update(21.f, 20.5f, 1.f);
  EXPECT_TRUE(pid.in_deadband());
  EXPECT_NEAR(0.05f, inside, 1e-5f);

  // 3 degrees below: outside, full gain plus the continuity offset.
  float outside = pid.update(21.f, 18.f, 1.f);
  EXPECT_FALSE(pid.in_deadband());
  EXPECT_GT(outside, 2.f);
}

TEST(PidCore, ZeroWidthDeadbandIsNeverInside) {
  PidCore pid;
  pid.set_gains(1.f, 0.f, 0.f);
  pid.update(21.f, 21.f, 1.f);
  EXPECT_FALSE(pid.in_deadband());
}

TEST(PidCore, OutputAveragingSmoothsAcrossSamples) {
  PidCore pid;
  pid.set_gains(1.f, 0.f, 0.f);
  pid.set_samples(2, 1);
  EXPECT_FLOAT_EQ(2.f, pid.update(22.f, 20.f, 1.f));
  // The average of the last two outputs: (2 + 0) / 2.
  EXPECT_FLOAT_EQ(1.f, pid.update(22.f, 22.f, 1.f));
}

TEST(PidCore, ResetDropsTheIntegralButKeepsTheTuning) {
  PidCore pid;
  pid.set_gains(0.f, 0.1f, 0.f);
  pid.set_integral_limits(-10.f, 10.f);
  pid.update(22.f, 20.f, 1.f);
  ASSERT_GT(pid.integral_term(), 0.f);

  pid.reset();
  pid.update(22.f, 20.f, 1.f);
  EXPECT_FLOAT_EQ(0.2f, pid.integral_term()) << "the gain survived, the accumulation did not";
}

TEST(HysteresisCore, HeatsBelowLowAndStopsAboveHigh) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 21.f);
  hyst.set_directions(true, false);

  EXPECT_EQ(HubAction::HEATING, hyst.update(HubMode::HEAT, 19.f));
  EXPECT_EQ(HubAction::HEATING, hyst.update(HubMode::HEAT, 20.5f)) << "latched between the points";
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT, 21.5f));
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT, 20.5f)) << "latched the other way";
}

TEST(HysteresisCore, HeatCoolIdlesBetweenThePoints) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 22.f);
  hyst.set_directions(true, true);

  EXPECT_EQ(HubAction::HEATING, hyst.update(HubMode::HEAT_COOL, 19.f));
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT_COOL, 21.f));
  EXPECT_EQ(HubAction::COOLING, hyst.update(HubMode::HEAT_COOL, 23.f));
}

TEST(HysteresisCore, WithoutTheDirectionItOnlyIdles) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 21.f);
  hyst.set_directions(false, true);
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::COOL, 19.f));
}

// Unknown is not "in range": IDLE here would leave a relay latched on a reading that never
// arrived.
TEST(HysteresisCore, NaNReportsOffNotIdle) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 21.f);
  hyst.set_directions(true, true);
  hyst.update(HubMode::HEAT_COOL, 19.f);
  EXPECT_EQ(HubAction::OFF, hyst.update(HubMode::HEAT_COOL, NAN));
}

TEST(HysteresisCore, ModeOffAlwaysReportsOff) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 21.f);
  hyst.set_directions(true, true);
  EXPECT_EQ(HubAction::OFF, hyst.update(HubMode::OFF, 5.f));
}

}  // namespace esphome::climate_hub::testing
