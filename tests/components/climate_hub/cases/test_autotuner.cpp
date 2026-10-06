// The relay-oscillation autotuner, against numbers worked out by hand and against upstream's own
// code: compiled a second time below, under another namespace and with its clock taken, it is
// the reference the port has to match number for number.
#include <cinttypes>
#include <numbers>
#include <vector>
#include "common.h"
#include "esphome/components/climate_hub/autotune.h"
#include "esphome/components/climate_hub/pid_autotuner.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/optional.h"
#include "room.h"

namespace esphome::climate_hub::testing {
// Upstream reads millis(); here it reads this.
uint32_t upstream_ms = 0;
}  // namespace esphome::climate_hub::testing

// clang-format off
#define millis() ::esphome::climate_hub::testing::upstream_ms
#define pid upstream_pid
#include "esphome/components/pid/pid_autotuner.cpp"
#undef pid
#undef millis
// clang-format on

namespace esphome::climate_hub::testing {
namespace {

// Upstream's tuner with what it keeps to itself in reach.
class Upstream : public upstream_pid::PIDAutotuner {
 public:
  float ku() const { return this->ku_; }
  float pu() const { return this->pu_; }
  PIDResult gains(float kp, float ki, float kd) { return this->calculate_pid_(kp, ki, kd); }
};

// The five rules, by the factors upstream prints them with.
struct Rule {
  AutotuneRule rule;
  float kp;
  float ki;
  float kd;
};
const Rule RULES[] = {
    {AutotuneRule::ZN_PI, 0.45f, 0.54f, 0.0f},         {AutotuneRule::ZN_PID, 0.6f, 1.2f, 0.075f},
    {AutotuneRule::PESSEN, 0.7f, 1.75f, 0.105f},       {AutotuneRule::SOME_OVERSHOOT, 0.333f, 0.667f, 0.111f},
    {AutotuneRule::NO_OVERSHOOT, 0.2f, 0.4f, 0.0625f},
};

// A reading every minute around a target of 20: the extremes and the crossings are where the
// comments say, so Ku and Pu can be worked out by hand.
struct Sample {
  uint32_t s;
  float value;
};
const Sample SWINGS[] = {
    {0, 20.0f},     // starts at the target: the relay off, the crossing detector below its band
    {60, 19.9f},    // crossing 1
    {120, 19.7f},   // switch 1 (on): the first phase's extreme, error 0 at 0 s
    {180, 19.6f},   // the coldest: error 0.4
    {240, 20.0f},   //
    {300, 20.4f},   // switch 2 (off), crossing 2: 240 s after the first
    {360, 20.5f},   // error -0.5
    {420, 20.0f},   //
    {480, 19.6f},   // switch 3, crossing 3: 180 s
    {540, 19.5f},   // error 0.5
    {600, 20.0f},   //
    {660, 20.5f},   // switch 4, crossing 4: 180 s
    {720, 20.6f},   // error -0.6
    {780, 20.0f},   //
    {840, 19.6f},   // switch 5, crossing 5: 180 s
    {900, 19.4f},   // error 0.6
    {960, 20.0f},   //
    {1020, 20.4f},  // switch 6, crossing 6: three of each extreme, and the end
};

// Fed one minute apart from `t0`; true when the tuner finished on the last one.
bool feed(PidAutotuner *tuner, const Sample *samples, size_t count, uint32_t t0 = 1000) {
  for (size_t i = 0; i < count; i++) {
    tuner->update(20.f, samples[i].value, t0 + samples[i].s * 1000);
    if (tuner->finished())
      return i + 1 == count;
  }
  return false;
}

PidAutotuner heating() {
  PidAutotuner tuner;
  tuner.config(0.f, 1.f);
  tuner.set_noiseband(0.25f);
  return tuner;
}

}  // namespace

// Ku = 4d / (πa): d is half the relay's span of 1, a half the swing upstream averages, the one
// between the second positive phase's largest error (0.5) and the third negative phase's smallest
// (-0.6). Pu is twice the mean of the five half-periods, (240 + 4 × 180) / 5 s.
TEST(PidAutotuner, KuAndPuAreUpstreamsFormulasWorkedByHand) {
  PidAutotuner tuner = heating();
  ASSERT_TRUE(feed(&tuner, SWINGS, std::size(SWINGS))) << "it stops at the sixth switch";
  EXPECT_EQ(6u, tuner.phase_count());
  const float a = (0.5f + 0.6f) / 2.f;
  EXPECT_NEAR(2.f / (std::numbers::pi_v<float> * a), tuner.ku(), 1e-5f);
  EXPECT_NEAR(2.f * (240.f + 4.f * 180.f) / 5.f, tuner.pu(), 1e-3f);
}

// The factors are upstream's: kp = f·Ku, ki = f·Ku/Pu, kd = f·Ku·Pu.
TEST(PidAutotuner, TheFiveRulesGiveTheirFactorsOfKuAndPu) {
  PidAutotuner tuner = heating();
  ASSERT_TRUE(feed(&tuner, SWINGS, std::size(SWINGS)));
  const float ku = tuner.ku();
  const float pu = tuner.pu();
  for (const Rule &rule : RULES) {
    AutotuneRun run(AutotuneDirection::HEAT, rule.rule, PidGains{}, 20.f, 1000);
    for (const Sample &sample : SWINGS)
      run.feed(sample.value, 1000 + sample.s * 1000);
    ASSERT_TRUE(run.found());
    bool clamped = true;
    const PidGains gains = run.result(&clamped);
    SCOPED_TRACE(enums::autotune_rule_to_string(rule.rule));
    EXPECT_FALSE(clamped);
    EXPECT_FLOAT_EQ(rule.kp * ku, gains.kp);
    EXPECT_FLOAT_EQ(rule.ki * ku / pu, gains.ki);
    EXPECT_FLOAT_EQ(rule.kd * ku * pu, gains.kd);
  }
}

// What the progress shows: the relay's side, the error it waits for, and the extremes in order,
// the first phase's too.
TEST(PidAutotuner, ReportsItsPhaseAndTheExtremesInOrder) {
  PidAutotuner tuner = heating();
  EXPECT_FALSE(tuner.started());
  EXPECT_FLOAT_EQ(0.f, tuner.target_error()) << "no side before the first sample";
  EXPECT_TRUE(std::isnan(tuner.swing_ratio()));

  EXPECT_EQ(0.f, tuner.update(20.f, 20.f, 1000)) << "at the target the relay starts off";
  EXPECT_TRUE(tuner.started());
  EXPECT_FALSE(tuner.positive());
  EXPECT_FLOAT_EQ(0.25f, tuner.target_error()) << "off, it waits for the room to fall 0.25 under";
  EXPECT_EQ(1.f, tuner.update(20.f, 19.7f, 61000));
  EXPECT_TRUE(tuner.positive());
  EXPECT_FLOAT_EQ(-0.25f, tuner.target_error());

  PidAutotuner full = heating();
  ASSERT_TRUE(feed(&full, SWINGS, std::size(SWINGS)));
  const std::vector<std::pair<uint32_t, float>> want = {{0, 0.f},    {180, 0.4f},  {360, -0.5f},
                                                        {540, 0.5f}, {720, -0.6f}, {900, 0.6f}};
  ASSERT_EQ(want.size(), full.extremes().size());
  for (size_t i = 0; i < want.size(); i++) {
    EXPECT_EQ(1000 + want[i].first * 1000, full.extremes()[i].ms) << i;
    EXPECT_NEAR(want[i].second, full.extremes()[i].error, 1e-5f) << i;
  }
  EXPECT_NEAR(0.9f / 1.2f, full.swing_ratio(), 1e-5f) << "0.9, 1.0, 1.1 and 1.2: the first phase left out";
  EXPECT_TRUE(full.symmetrical()) << "180 against 240 s";
}

// Finished, it holds its numbers and drives nothing.
TEST(PidAutotuner, AFinishedTunerChangesNothing) {
  PidAutotuner tuner = heating();
  ASSERT_TRUE(feed(&tuner, SWINGS, std::size(SWINGS)));
  const float ku = tuner.ku();
  EXPECT_EQ(0.f, tuner.update(20.f, 15.f, 2000000));
  EXPECT_EQ(6u, tuner.phase_count());
  EXPECT_EQ(ku, tuner.ku());
}

// Cooling alone swings between off and full cooling, so d is a half as for heating.
TEST(PidAutotuner, ConfigNarrowsTheRelayToOneDirection) {
  PidAutotuner cooling;
  cooling.config(-1.f, 0.f);
  cooling.set_noiseband(0.25f);
  EXPECT_EQ(-1.f, cooling.update(20.f, 21.f, 1000)) << "above the target a cooler runs";
  EXPECT_EQ(0.f, cooling.update(20.f, 19.f, 2000));

  PidAutotuner both;
  both.set_noiseband(0.25f);
  EXPECT_EQ(-1.f, both.update(20.f, 21.f, 1000)) << "upstream's default spans -1 to 1";
}

// Upstream measures only the half-periods; how far apart the swings are tells whether
// something disturbed the room, which its convergence check never does.
TEST(PidAutotuner, UnevenSwingsAndUnevenHalfPeriodsAreTold) {
  const Sample samples[] = {
      {0, 20.0f},   {60, 19.9f},  {120, 19.7f}, {180, 19.6f},  {240, 20.4f},  {300, 20.5f},  {360, 19.6f},
      {420, 19.5f}, {900, 20.3f}, {960, 21.5f}, {1020, 19.7f}, {1080, 19.6f}, {1140, 20.4f},
  };
  PidAutotuner tuner = heating();
  ASSERT_TRUE(feed(&tuner, samples, std::size(samples)));
  EXPECT_FALSE(tuner.symmetrical()) << "120 s against 540 s";
  EXPECT_LT(tuner.swing_ratio(), AUTOTUNE_EVEN_RATIO) << "0.9 against 2.0";
}

namespace {

// The same room driven by the port and fed to both; each sample's output, the pass that
// finishes, Ku, Pu and every rule's gains have to be the same floats.
void expect_upstreams_numbers(const RoomModel &model, float setpoint, bool cooling, uint32_t t0) {
  Room room(model);
  PidAutotuner port;
  Upstream upstream{};
  port.config(cooling ? -1.f : 0.f, cooling ? 0.f : 1.f);
  upstream.config(cooling ? -1.f : 0.f, cooling ? 0.f : 1.f);
  port.set_noiseband(0.25f);
  upstream.set_noiseband(0.25f);

  bool relay = false;
  float reading = room.reading();
  const auto step_ms = static_cast<uint32_t>(model.step_s * 1000.f);
  for (uint32_t n = 0; n < 48u * 3600u * 1000u / step_ms; n++) {
    const uint32_t now = t0 + n * step_ms;
    upstream_ms = now;
    const auto want = upstream.update(setpoint, reading);
    const float got = port.update(setpoint, reading, now);
    ASSERT_EQ(want.result_params.has_value(), port.finished()) << "sample " << n;
    if (port.finished()) {
      EXPECT_GE(port.phase_count(), 6u);
      EXPECT_EQ(upstream.ku(), port.ku());
      EXPECT_EQ(upstream.pu(), port.pu());
      for (const Rule &rule : RULES) {
        const auto theirs = upstream.gains(rule.kp, rule.ki, rule.kd);
        const PidAutotuner::Gains ours = port.gains(rule.kp, rule.ki, rule.kd);
        EXPECT_EQ(theirs.kp, ours.kp);
        EXPECT_EQ(theirs.ki, ours.ki);
        EXPECT_EQ(theirs.kd, ours.kd);
      }
      // Upstream's own answer is its Ziegler-Nichols PID.
      EXPECT_EQ(want.result_params->kp, port.gains(0.6f, 1.2f, 0.075f).kp);
      return;
    }
    ASSERT_EQ(want.output, got) << "sample " << n;
    relay = cooling ? got < 0.f : got > 0.f;
    reading = room.step(relay);
  }
  FAIL() << "no result in 48 h";
}

}  // namespace

TEST(PidAutotuner, MatchesUpstreamOnARoomWithRadiators) {
  expect_upstreams_numbers(radiator_room(), 21.f, false, 5000);
}

TEST(PidAutotuner, MatchesUpstreamOnAFloorHeating) { expect_upstreams_numbers(floor_heating(), 21.f, false, 5000); }

TEST(PidAutotuner, MatchesUpstreamOnANoisyProbe) {
  RoomModel model = radiator_room();
  model.noise = 0.08f;
  model.resolution = 0.0625f;
  expect_upstreams_numbers(model, 21.f, false, 5000);
}

TEST(PidAutotuner, MatchesUpstreamCooling) {
  RoomModel model = radiator_room();
  model.start = 27.f;
  model.outside = 32.f;
  model.drive = -10.f;
  expect_upstreams_numbers(model, 24.f, true, 5000);
}

// Upstream takes a crossing at 0 ms for none yet; the port does not, and differs from it there
// alone: the clock it is handed may well read 0.
TEST(PidAutotuner, ACrossingAtZeroMillisecondsCounts) {
  PidAutotuner tuner = heating();
  // Shifted so that crossing 1 lands on 0 ms, the first sample before it on the wrap.
  for (const Sample &sample : SWINGS)
    tuner.update(20.f, sample.value, sample.s * 1000u - 60000u);
  ASSERT_TRUE(tuner.finished());
  EXPECT_NEAR(2.f * (240.f + 4.f * 180.f) / 5.f, tuner.pu(), 1e-3f) << "five half-periods, the first from 0 ms";
}

}  // namespace esphome::climate_hub::testing
