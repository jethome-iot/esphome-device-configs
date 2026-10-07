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

  for (int i = 0; i < 20; i++)
    pid.update(20.f, 22.f, 1.f);
  EXPECT_FLOAT_EQ(-1.f, pid.integral_term()) << "and at min_integral, the other way";
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
  // The oldest drops out of the window: (0 + 4) / 2.
  EXPECT_FLOAT_EQ(2.f, pid.update(22.f, 18.f, 1.f));
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

// After a pause: the next update averages nothing from before it in, and goes on from the
// integral held.
TEST(PidCore, ForgetSamplesKeepsTheIntegral) {
  PidCore pid;
  pid.set_gains(1.f, 0.1f, 1.f);
  pid.set_integral_limits(-10.f, 10.f);
  pid.set_samples(4, 4);
  pid.update(22.f, 20.f, 1.f);
  pid.update(22.f, 19.f, 1.f);
  ASSERT_FLOAT_EQ(1.5f, pid.derivative_term());
  ASSERT_FLOAT_EQ(0.5f, pid.integral_term());

  pid.forget_samples();
  EXPECT_FLOAT_EQ(2.5f, pid.update(22.f, 20.f, 0.f)) << "2 proportional and the 0.5 held, averaged with nothing";
  EXPECT_FLOAT_EQ(0.5f, pid.integral_term());
  EXPECT_EQ(0.f, pid.derivative_term());
}

// New limits take a kept integral in at once: clamped only at the next update, it would first
// move from where it was and land on the limit.
TEST(PidCore, NewIntegralLimitsClampTheKeptIntegral) {
  PidCore pid;
  pid.set_gains(0.f, 0.1f, 0.f);
  pid.set_integral_limits(-10.f, 10.f);
  for (int i = 0; i < 4; i++)
    pid.update(22.f, 20.f, 1.f);
  ASSERT_FLOAT_EQ(0.8f, pid.integral_term());

  pid.set_integral_limits(-1.f, 0.5f);
  EXPECT_FLOAT_EQ(0.5f, pid.integral_term());
  pid.update(20.f, 21.f, 1.f);
  EXPECT_FLOAT_EQ(0.4f, pid.integral_term());
  pid.set_integral_limits(0.6f, 1.f);
  EXPECT_FLOAT_EQ(0.6f, pid.integral_term()) << "and from below";
}

// Every slot of the pool has a controller from boot, running or not: its smoothing windows
// take memory only once they are sized, as a controller starts.
TEST(PidCore, HoldsNoHeapUntilItsWindowsAreSized) {
#ifndef __SANITIZE_ADDRESS__
  GTEST_SKIP() << "only AddressSanitizer counts the heap exactly";
#else
  const size_t before = heap_in_use();
  {
    PidCore pid;
    EXPECT_EQ(before, heap_in_use());
    pid.set_samples(1, 1);
    pid.set_deadband(0.f, 0.f, 0.f, 0.f, 0.f, 1);
    EXPECT_EQ(before, heap_in_use()) << "a window of one keeps nothing";
    pid.set_samples(1, 8);
    EXPECT_GT(heap_in_use(), before);
  }
  EXPECT_EQ(before, heap_in_use());
#endif
}

// The output window is shared with the deadband's: a shorter one there drops the oldest.
TEST(PidCore, AShorterWindowDropsTheOldestSamples) {
  PidCore pid;
  pid.set_gains(1.f, 0.f, 0.f);
  pid.set_samples(4, 1);
  pid.set_deadband(-1.f, 1.f, 1.f, 0.f, 0.f, 2);
  EXPECT_FLOAT_EQ(4.f, pid.update(22.f, 18.f, 1.f));
  EXPECT_FLOAT_EQ(5.f, pid.update(22.f, 16.f, 1.f));
  EXPECT_FLOAT_EQ(6.f, pid.update(22.f, 14.f, 1.f));
  // Inside the deadband: the newest outside value and this one, (8 + 0.5) / 2.
  EXPECT_FLOAT_EQ(4.25f, pid.update(22.f, 21.5f, 1.f));
  EXPECT_TRUE(pid.in_deadband());
}

// The output window serves the deadband too, so it is sized for the longer of the two as the
// controller starts: a pass inside a deadband averaging over more samples allocates nothing.
TEST(PidCore, ADeadbandWindowLongerThanTheOutputWindowWins) {
#ifndef __SANITIZE_ADDRESS__
  GTEST_SKIP() << "only AddressSanitizer counts the heap exactly";
#else
  PidCore pid;
  pid.set_gains(1.f, 0.f, 0.f);
  pid.set_samples(1, 1);
  pid.set_deadband(-1.f, 1.f, 1.f, 0.f, 0.f, 4);
  const size_t sized = heap_in_use();
  float out[5];
  int i = 0;
  for (float reading : {21.5f, 21.75f, 21.25f, 22.f, 21.5f})
    out[i++] = pid.update(22.f, reading, 1.f);
  EXPECT_EQ(sized, heap_in_use());
  EXPECT_TRUE(pid.in_deadband());
  EXPECT_FLOAT_EQ(0.5f, out[0]);
  EXPECT_FLOAT_EQ(0.375f, out[1]) << "(0.5 + 0.25) / 2";
  EXPECT_FLOAT_EQ(0.5f, out[2]) << "(0.5 + 0.25 + 0.75) / 3";
  EXPECT_FLOAT_EQ(0.375f, out[3]) << "(0.5 + 0.25 + 0.75 + 0) / 4";
  EXPECT_FLOAT_EQ(0.375f, out[4]) << "(0.25 + 0.75 + 0 + 0.5) / 4: the oldest dropped";
#endif
}

// A window made wider keeps what it holds.
TEST(PidCore, AWiderWindowKeepsWhatItHolds) {
  PidCore pid;
  pid.set_gains(1.f, 0.f, 0.f);
  pid.set_samples(2, 1);
  pid.update(22.f, 20.f, 1.f);
  pid.update(22.f, 18.f, 1.f);
  pid.set_samples(4, 1);
  EXPECT_FLOAT_EQ(4.f, pid.update(22.f, 16.f, 1.f)) << "(2 + 4 + 6) / 3";
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

TEST(HysteresisCore, CoolsAboveHighAndStopsBelowLow) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 21.f);
  hyst.set_directions(false, true);

  EXPECT_EQ(HubAction::COOLING, hyst.update(HubMode::COOL, 22.f));
  EXPECT_EQ(HubAction::COOLING, hyst.update(HubMode::COOL, 20.5f)) << "latched between the points";
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::COOL, 19.f));
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT, 19.f)) << "no heating relay to call on";
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT, 22.f)) << "and too hot is not its to fix in HEAT";
}

// Both directions wired, the mode still decides: COOL below the band idles, and so does HEAT
// above it.
TEST(HysteresisCore, ASingleDirectionModeLeavesTheOtherAlone) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 21.f);
  hyst.set_directions(true, true);
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::COOL, 19.f));
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT, 22.f)) << "nor does HEAT cool";
}

// Nothing latched, as the core starts, after a reset or after an unknown reading, is idle
// inside the band: off belongs to mode off.
TEST(HysteresisCore, NothingLatchedInsideTheBandIsIdle) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 21.f);
  hyst.set_directions(true, false);
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT, 20.5f));

  hyst.update(HubMode::HEAT, 19.f);
  hyst.reset();
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT, 20.5f)) << "after a reset";
  ASSERT_EQ(HubAction::OFF, hyst.update(HubMode::HEAT, NAN));
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT, 20.5f)) << "after a NaN";
  ASSERT_EQ(HubAction::OFF, hyst.update(HubMode::OFF, 20.5f));
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT, 20.5f)) << "after mode off";
}

// A switching point that is not a number is as unknown as a reading that is not one.
TEST(HysteresisCore, AMissingSwitchingPointReportsOff) {
  HysteresisCore hyst;
  hyst.set_directions(true, true);
  hyst.set_setpoints(NAN, 21.f);
  EXPECT_EQ(HubAction::OFF, hyst.update(HubMode::HEAT_COOL, 18.f));
  hyst.set_setpoints(20.f, NAN);
  EXPECT_EQ(HubAction::OFF, hyst.update(HubMode::HEAT_COOL, 18.f));
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

// Seeded, the latch holds the seeded action until a switching point is crossed.
TEST(HysteresisCore, ASeededLatchHoldsInsideTheBand) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 21.f);
  hyst.set_directions(true, false);
  hyst.seed(HubAction::HEATING);
  EXPECT_EQ(HubAction::HEATING, hyst.update(HubMode::HEAT, 20.5f));
  EXPECT_EQ(HubAction::IDLE, hyst.update(HubMode::HEAT, 21.5f));
}

TEST(HysteresisCore, ModeOffAlwaysReportsOff) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 21.f);
  hyst.set_directions(true, true);
  EXPECT_EQ(HubAction::OFF, hyst.update(HubMode::OFF, 5.f));
}

}  // namespace esphome::climate_hub::testing
