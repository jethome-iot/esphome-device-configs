// The two control laws, with dt injected. PidCore is term for term ESPHome's pid controller
// until it is given an output range: compiled a second time below, under another namespace and
// with its clock taken, upstream's is the reference it has to match without one.
#include "common.h"
#include "esphome/components/climate_hub/hysteresis_core.h"
#include "esphome/components/climate_hub/pid_core.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome::climate_hub::testing {
// Upstream's controller reads millis(); here it reads this.
uint32_t upstream_pid_ms = 0;
}  // namespace esphome::climate_hub::testing

// clang-format off
#define millis() ::esphome::climate_hub::testing::upstream_pid_ms
#define pid upstream_pid
#include "esphome/components/pid/pid_controller.cpp"
#undef pid
#undef millis
// clang-format on

namespace esphome::upstream_pid {
// The controller keeps its tuning to PIDClimate, its friend; this one sets it as that one's
// setters do and reads its terms back.
class PIDClimate {
 public:
  static void tune(PIDController &c, float kp, float ki, float kd, int output_samples, int derivative_samples) {
    c.kp_ = kp;
    c.ki_ = ki;
    c.kd_ = kd;
    c.min_integral_ = -1.f;
    c.max_integral_ = 1.f;
    c.output_samples_ = output_samples;
    c.derivative_samples_ = derivative_samples;
    if (derivative_samples > 1)
      c.derivative_window_.init(derivative_samples);
  }
  static void deadband(PIDController &c, float low, float high, float kp_multiplier, float ki_multiplier,
                       float kd_multiplier, int output_samples) {
    c.threshold_low_ = low;
    c.threshold_high_ = high;
    c.kp_multiplier_ = kp_multiplier;
    c.ki_multiplier_ = ki_multiplier;
    c.kd_multiplier_ = kd_multiplier;
    c.deadband_output_samples_ = output_samples;
    const int size = std::max(c.output_samples_, output_samples);
    if (size > 1)
      c.output_window_.init(size);
  }
  static float proportional(const PIDController &c) { return c.proportional_term_; }
  static float integral(const PIDController &c) { return c.integral_term_; }
  static float derivative(const PIDController &c) { return c.derivative_term_; }
};
}  // namespace esphome::upstream_pid

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

// Without a range, upstream's numbers to the bit, the integral wound past the output's reach
// as upstream winds it: a target step, a long way short, a setpoint move for the derivative to
// ignore, and the deadband.
TEST(PidCore, WithoutARangeItIsUpstreamsLawToTheBit) {
  PidCore port;
  upstream_pid::PIDController upstream;
  port.set_gains(0.6f, 0.0025f, 30.f);
  port.set_integral_limits(-1.f, 1.f);
  port.set_samples(3, 4);
  port.set_deadband(-0.25f, 0.25f, 0.5f, 0.2f, 0.f, 5);
  upstream_pid::PIDClimate::tune(upstream, 0.6f, 0.0025f, 30.f, 3, 4);
  upstream_pid::PIDClimate::deadband(upstream, -0.25f, 0.25f, 0.5f, 0.2f, 0.f, 5);
  port.set_starting_integral_term(0.3f);
  upstream.set_starting_integral_term(0.3f);

  upstream_pid_ms = 5000;
  float setpoint = 21.f;
  float reading = 17.f;
  bool saturated = false;
  bool deadband = false;
  for (int n = 0; n < 600; n++) {
    // Uneven passes, as the loop gives them; upstream's first one sees no time.
    const uint32_t step_ms = n == 0 ? 0u : 1000u * static_cast<uint32_t>(5 + n % 7);
    upstream_pid_ms += step_ms;
    if (n == 200)
      setpoint = 18.f;
    if (n == 400)
      setpoint = 21.5f;
    const float want = upstream.update(setpoint, reading);
    const float got = port.update(setpoint, reading, static_cast<float>(step_ms) / 1000.0f);
    ASSERT_EQ(want, got) << "pass " << n;
    ASSERT_EQ(upstream_pid::PIDClimate::proportional(upstream), port.proportional_term()) << "pass " << n;
    ASSERT_EQ(upstream_pid::PIDClimate::integral(upstream), port.integral_term()) << "pass " << n;
    ASSERT_EQ(upstream_pid::PIDClimate::derivative(upstream), port.derivative_term()) << "pass " << n;
    saturated |= got > 1.f && port.error() > 0.f;
    deadband |= port.in_deadband();
    // A first-order room, the output clipped as a relay clips it.
    reading += (std::clamp(got, 0.f, 1.f) * 6.f - (reading - 10.f) / 2.f) * 30.f / 3600.f;
  }
  EXPECT_TRUE(saturated) << "the case integrates past the range";
  EXPECT_TRUE(deadband) << "and goes through the deadband";
}

// A range of NaN at both ends, set or left, integrates whatever the output.
TEST(PidCore, ANaNRangeTakesTheBoundAway) {
  PidCore pid;
  pid.set_gains(1.f, 0.1f, 0.f);
  pid.set_integral_limits(-10.f, 10.f);
  pid.set_output_range(0.f, 1.f);
  pid.set_output_range(NAN, NAN);
  pid.update(25.f, 20.f, 1.f);
  EXPECT_FLOAT_EQ(0.5f, pid.integral_term());
}

// Heating: the output reads the share of the period the relay is closed, so nothing more is
// integrated while it asks for all of it, nor less while it asks for none.
TEST(PidCore, NoIntegrationThatPushesTheOutputFurtherPastItsRange) {
  PidCore pid;
  pid.set_gains(0.1f, 0.1f, 0.f);
  pid.set_integral_limits(-10.f, 10.f);
  pid.set_output_range(0.f, 1.f);

  EXPECT_FLOAT_EQ(2.f, pid.update(40.f, 20.f, 1.f));
  pid.update(40.f, 20.f, 1.f);
  EXPECT_FLOAT_EQ(0.f, pid.integral_term()) << "full on already";
  pid.update(20.f, 40.f, 1.f);
  EXPECT_FLOAT_EQ(0.f, pid.integral_term()) << "full off already";
  // Inside the range it integrates again, up and down.
  EXPECT_FLOAT_EQ(0.4f, pid.update(22.f, 20.f, 1.f)) << "0.2 proportional, 0.2 integrated";
  EXPECT_FLOAT_EQ(0.f, pid.update(22.f, 23.f, 1.f)) << "-0.1 and 0.1: 0.2 less 0.1";
  EXPECT_FLOAT_EQ(0.1f, pid.integral_term());
}

// Exactly at an end is past it, whichever way: the step would take it further.
TEST(PidCore, AnOutputAtTheEndOfTheRangeIsPastIt) {
  PidCore pid;
  pid.set_gains(0.5f, 0.1f, 0.f);
  pid.set_integral_limits(-10.f, 10.f);
  pid.set_output_range(0.f, 1.f);
  EXPECT_FLOAT_EQ(1.f, pid.update(22.f, 20.f, 1.f));
  EXPECT_FLOAT_EQ(0.f, pid.integral_term()) << "at the top";

  PidCore floor;
  floor.set_gains(0.f, 0.1f, 0.f);
  floor.set_output_range(0.f, 1.f);
  EXPECT_FLOAT_EQ(0.f, floor.update(20.f, 21.f, 1.f));
  EXPECT_FLOAT_EQ(0.f, floor.integral_term()) << "at the floor";
}

// Past an end, a step back towards the range is taken: the integral unwinds.
TEST(PidCore, AnIntegralPastTheRangeUnwinds) {
  PidCore pid;
  pid.set_gains(0.f, 0.1f, 0.f);
  pid.set_integral_limits(-10.f, 10.f);
  pid.set_output_range(0.f, 1.f);
  pid.set_starting_integral_term(1.5f);
  pid.update(20.f, 21.f, 1.f);
  EXPECT_FLOAT_EQ(1.4f, pid.integral_term()) << "from above";
  pid.set_starting_integral_term(-0.5f);
  pid.update(21.f, 20.f, 1.f);
  EXPECT_FLOAT_EQ(-0.4f, pid.integral_term()) << "and from below";
}

// The output the step is judged on is the one before it: a step that only takes it past the end
// is still taken.
TEST(PidCore, TheStepThatCrossesTheEndIsTaken) {
  PidCore pid;
  pid.set_gains(0.f, 0.1f, 0.f);
  pid.set_integral_limits(-10.f, 10.f);
  pid.set_output_range(0.f, 1.f);
  pid.set_starting_integral_term(0.95f);
  EXPECT_FLOAT_EQ(1.05f, pid.update(21.f, 20.f, 1.f));
  pid.update(21.f, 20.f, 1.f);
  EXPECT_FLOAT_EQ(1.05f, pid.integral_term()) << "and then no more";
}

// The derivative counts in that output, this pass's own: a reading falling fast has the output
// full on with no proportional term at all.
TEST(PidCore, TheDerivativeOfThisPassCounts) {
  PidCore pid;
  pid.set_gains(0.f, 0.1f, 1.f);
  pid.set_integral_limits(-10.f, 10.f);
  pid.set_output_range(0.f, 1.f);
  pid.set_samples(1, 1);
  pid.update(22.f, 22.f, 1.f);
  EXPECT_FLOAT_EQ(2.f, pid.update(22.f, 20.f, 1.f));
  EXPECT_FLOAT_EQ(0.f, pid.integral_term()) << "2 from the derivative alone";

  // Rising fast while still short, the derivative brings it back inside, and it integrates.
  PidCore inside;
  inside.set_gains(2.f, 0.1f, 1.f);
  inside.set_integral_limits(-10.f, 10.f);
  inside.set_output_range(0.f, 1.f);
  inside.set_samples(1, 1);
  inside.update(22.f, 19.5f, 1.f);
  ASSERT_FLOAT_EQ(0.f, inside.integral_term()) << "5 from the proportional term";
  EXPECT_FLOAT_EQ(0.6f, inside.update(22.f, 21.f, 1.f)) << "2 - 1.5 before the step, then 0.1 on";
  EXPECT_FLOAT_EQ(0.1f, inside.integral_term());
}

// Cooling: the output runs from -1, full on, to 0.
TEST(PidCore, ACoolingRangeIsTheMirror) {
  PidCore pid;
  pid.set_gains(0.5f, 0.1f, 0.f);
  pid.set_integral_limits(-10.f, 10.f);
  pid.set_output_range(-1.f, 0.f);

  pid.update(20.f, 25.f, 1.f);
  EXPECT_FLOAT_EQ(0.f, pid.integral_term()) << "full on already";
  pid.update(20.f, 19.f, 1.f);
  EXPECT_FLOAT_EQ(0.f, pid.integral_term()) << "too cold, the cooler off already";
  EXPECT_FLOAT_EQ(-0.3f, pid.update(20.f, 20.5f, 1.f));
  EXPECT_FLOAT_EQ(-0.05f, pid.integral_term()) << "inside, integrating";
}

// The deadband's ki multiplier and the integral limits apply as before.
TEST(PidCore, TheDeadbandAndTheLimitsStillApply) {
  PidCore pid;
  pid.set_gains(0.2f, 0.1f, 0.f);
  pid.set_integral_limits(-1.f, 0.12f);
  pid.set_output_range(0.f, 1.f);
  pid.set_deadband(-1.f, 1.f, 1.f, 0.5f, 0.f, 1);
  pid.update(21.f, 20.5f, 1.f);
  ASSERT_TRUE(pid.in_deadband());
  EXPECT_FLOAT_EQ(0.025f, pid.integral_term()) << "half a degree for 1 s at 0.1, halved";
  pid.update(25.f, 20.f, 1.f);
  EXPECT_FLOAT_EQ(0.025f, pid.integral_term()) << "outside the deadband and past the range";
  pid.update(21.f, 20.f, 1.f);
  EXPECT_FLOAT_EQ(0.12f, pid.integral_term()) << "inside the range, held at max_integral";
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

// Both relays wired, a mode still latches its one direction between the points.
TEST(HysteresisCore, BothRelaysLatchBetweenThePointsInEitherMode) {
  HysteresisCore hyst;
  hyst.set_setpoints(20.f, 22.f);
  hyst.set_directions(true, true);

  EXPECT_EQ(HubAction::HEATING, hyst.update(HubMode::HEAT, 19.f));
  EXPECT_EQ(HubAction::HEATING, hyst.update(HubMode::HEAT, 21.f)) << "latched, not idle";
  hyst.reset();
  EXPECT_EQ(HubAction::COOLING, hyst.update(HubMode::COOL, 23.f));
  EXPECT_EQ(HubAction::COOLING, hyst.update(HubMode::COOL, 21.f)) << "latched, not idle";
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
  EXPECT_EQ(HubAction::OFF, hyst.update(HubMode::HEAT, 18.f));
  hyst.set_setpoints(20.f, NAN);
  EXPECT_EQ(HubAction::OFF, hyst.update(HubMode::COOL, 18.f));
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
  hyst.update(HubMode::HEAT, 19.f);
  EXPECT_EQ(HubAction::OFF, hyst.update(HubMode::HEAT, NAN));
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
