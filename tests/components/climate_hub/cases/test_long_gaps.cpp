// Gaps a device takes weeks to reach: a sensor silent or a relay left alone past the point where
// a 32-bit millis() wraps, about 49.7 days, and a PID that stood still. The hub's clock is the
// test's.
#include "common.h"

namespace esphome::climate_hub::testing {
namespace {

constexpr uint64_t TEN_DAYS_MS = 10ull * 86400 * 1000;

class LongGaps : public HubTest {
 protected:
  // Heats from relay_1 off the room probe, which goes stale after 10 s; a pass every second.
  static ClimateConfig base(ControlKind kind) {
    ClimateConfig c;
    c.name = "Gaps";
    c.kind = kind;
    c.sensor_id = "room";
    c.heat.relay_id = "relay_1";
    c.heat.period_s = 10.f;
    c.heat.min_on_s = 0.f;
    c.heat.min_off_s = 0.f;
    c.mode = HubMode::HEAT;
    c.update_interval_s = 1.f;
    c.safety.sensor_timeout_s = 10.f;
    // Switching points at 20 and 21; a PID 5 degrees short of 25.
    c.setpoint = 20.5f;
    c.bang_bang.below = 0.5f;
    c.bang_bang.above = 0.5f;
    return c;
  }

  // An integral that grows by 0.05 a second at 20 degrees and nothing else.
  static ClimateConfig integrating() {
    ClimateConfig c = base(ControlKind::PID);
    c.setpoint = 25.f;
    c.pid.kp = 0.f;
    c.pid.ki = 0.01f;
    return c;
  }

  // Creates and starts `config`, with a first reading when `temperature` is a number.
  ControllerRuntime *start(const ClimateConfig &config, float temperature = NAN) {
    this->id_ = this->create(config).id;
    if (!std::isnan(temperature))
      entities().room.publish_state(temperature);
    return hub().runtime_of(this->id_);
  }

  // One pass of the hub's loop at `ms`.
  static void tick(uint64_t ms) {
    hub().ms = ms;
    hub().loop();
  }

  void mode(climate::ClimateMode mode) {
    auto call = hub().entity_of(this->id_)->make_call();
    call.set_mode(mode);
    call.perform();
  }

  std::string id_;
};

// --- Silence ---

// Counted in 32 bits, 49.7 days and a second of silence would read as a second, and the
// thermostat would heat on a reading that old.
TEST_F(LongGaps, ASensorSilentPastTheMillisWrapStaysStale) {
  ControllerRuntime *rt = this->start(base(ControlKind::BANG_BANG), 18.f);
  tick(100000);
  ASSERT_TRUE(entities().relay1.state);
  tick(111000);
  ASSERT_EQ(HubFault::SENSOR_STALE, rt->fault());

  tick(100000 + MILLIS_WRAP + 1000);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault());
  EXPECT_EQ(HubAction::OFF, rt->action());
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_NEAR(4294968.f, rt->sensor_age_s(hub().ms), 1.f) << "49.7 days and a second";

  entities().room.publish_state(18.f);
  tick(hub().ms + 1000);
  EXPECT_EQ(HubFault::NONE, rt->fault()) << "only a new reading ends it";
  EXPECT_TRUE(entities().relay1.state);
}

// The wait for a first reading does not start over either: still stale, not waiting again.
TEST_F(LongGaps, TheWaitForAFirstReadingStaysStalePastTheMillisWrap) {
  ControllerRuntime *rt = this->start(base(ControlKind::BANG_BANG));
  tick(110001);
  ASSERT_EQ(HubFault::SENSOR_STALE, rt->fault());

  tick(100000 + MILLIS_WRAP + 5000);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault());
  EXPECT_EQ(HubAction::OFF, rt->action());
  EXPECT_TRUE(std::isnan(rt->sensor_age_s(hub().ms)));
}

// What the hub heard before the wrap is handed to a thermostat that starts after it as the old
// reading it is.
TEST_F(LongGaps, AReadingHeardBeforeTheMillisWrapIsStaleAtAStart) {
  ClimateConfig config = base(ControlKind::BANG_BANG);
  // The hub hears the probe only once a thermostat on it has run.
  ClimateConfig listener = config;
  listener.name = "Listener";
  listener.heat.relay_id = "relay_2";
  this->create(listener);
  entities().room.publish_state(18.f);
  ASSERT_TRUE(hub().remove("listener").ok);

  hub().ms += MILLIS_WRAP + 1000;
  ControllerRuntime *rt = this->start(config);
  EXPECT_FLOAT_EQ(18.f, hub().entity_of(this->id_)->current_temperature) << "shown";
  tick(hub().ms);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault()) << "not acted on";
  EXPECT_FALSE(entities().relay1.state);
}

// --- The rest of the clock ---

// 3 s in, a 32-bit millis() would wrap: the PWM's rhythm goes on through it, 5 s on in every 10.
TEST_F(LongGaps, ThePwmKeepsItsRhythmAcrossTheMillisWrap) {
  ClimateConfig config = base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.f;
  config.safety.sensor_timeout_s = 60.f;
  hub().ms = MILLIS_WRAP - 3000;
  ControllerRuntime *rt = this->start(config, 20.f);

  std::vector<bool> states;
  for (uint64_t t = MILLIS_WRAP - 3000; t <= MILLIS_WRAP + 12000; t += 1000) {
    tick(t);
    states.push_back(entities().relay1.state);
  }
  EXPECT_EQ((std::vector<bool>{true, true, true, true, true, false, false, false, false, false, true, true, true, true,
                               true, false}),
            states);
  EXPECT_EQ(HubFault::NONE, rt->fault());
}

// A change from Home Assistant still waits out the 3 s write debounce past the wrap.
TEST_F(LongGaps, AChangeWaitsOutItsDebouncePastTheMillisWrap) {
  hub().ms = MILLIS_WRAP + 1000;
  this->start(base(ControlKind::BANG_BANG), 18.f);
  this->mode(climate::CLIMATE_MODE_OFF);
  ASSERT_TRUE(hub().dirty(this->id_));
  tick(MILLIS_WRAP + 3999);
  EXPECT_TRUE(hub().dirty(this->id_));
  tick(MILLIS_WRAP + 4000);
  EXPECT_FALSE(hub().dirty(this->id_));
}

// The hub's own clock is the system's. A host up for less than 49.7 days cannot tell
// millis_64() from millis() here: that it is millis_64() rests on reading climate_hub.cpp.
TEST_F(LongGaps, TheHubCountsOnMillis64) {
  const uint64_t before = millis_64();
  const uint64_t now = hub().ClimateHub::now_ms();
  EXPECT_LE(before, now);
  EXPECT_LE(now, millis_64());
}

// --- Relays ---

// Ten minutes of min_off do not hold a relay that has been open for 49.7 days and a second: in
// 32 bits that would read as a second, and the thermostat would wait the dwell out again.
TEST_F(LongGaps, ARelayLeftOpenPastTheMillisWrapClosesAtOnce) {
  ClimateConfig config = base(ControlKind::BANG_BANG);
  config.heat.min_off_s = 600.f;
  // Past the min_off the boot started.
  hub().ms = 1000000;
  this->start(config, 18.f);
  tick(1000000);
  ASSERT_TRUE(entities().relay1.state);
  this->mode(climate::CLIMATE_MODE_OFF);
  tick(1001000);
  ASSERT_FALSE(entities().relay1.state);

  hub().ms = 1001000 + MILLIS_WRAP + 1000;
  entities().room.publish_state(18.f);
  this->mode(climate::CLIMATE_MODE_HEAT);
  tick(hub().ms);
  EXPECT_TRUE(entities().relay1.state);
}

// A relay counts as opened at boot: 49.7 days of uptime and a second later, that is long past.
TEST_F(LongGaps, ARelayOpenedAtBootClosesAtOnceOnALongUptime) {
  ClimateConfig config = base(ControlKind::BANG_BANG);
  config.heat.min_off_s = 600.f;
  hub().ms = MILLIS_WRAP + 1000;
  this->start(config, 18.f);
  tick(hub().ms);
  EXPECT_TRUE(entities().relay1.state);
}

// The switching a thermostat left a relay at is as old for the next one on it.
TEST_F(LongGaps, ARelayLetGoBeforeTheMillisWrapClosesAtOnceAfterIt) {
  ClimateConfig config = base(ControlKind::BANG_BANG);
  config.heat.min_off_s = 600.f;
  hub().ms = 1000000;
  this->start(config, 18.f);
  tick(1000000);
  ASSERT_TRUE(entities().relay1.state);
  ASSERT_TRUE(hub().remove(this->id_).ok);
  ASSERT_FALSE(entities().relay1.state);

  hub().ms += MILLIS_WRAP + 1000;
  this->start(config, 18.f);
  tick(hub().ms);
  EXPECT_TRUE(entities().relay1.state);
}

// --- A PID after a pause ---

// Ten days in mode off: the first pass after it integrates nothing and takes no derivative, so
// the integral goes on from where it stood rather than from its limit.
TEST_F(LongGaps, APidStartsAfreshAfterModeOff) {
  ClimateConfig config = integrating();
  config.safety.sensor_timeout_s = 60.f;
  config.pid.kd = 1.f;
  ControllerRuntime *rt = this->start(config, 20.f);
  tick(100000);
  tick(101000);
  ASSERT_NEAR(0.05f, rt->pid().integral_term(), 1e-5f);

  this->mode(climate::CLIMATE_MODE_OFF);
  tick(102000);
  hub().ms = 101000 + TEN_DAYS_MS;
  // A degree colder: over ten days the derivative is next to nothing, over no time it is none.
  entities().room.publish_state(19.f);
  this->mode(climate::CLIMATE_MODE_HEAT);
  tick(hub().ms);
  EXPECT_NEAR(0.05f, rt->pid().integral_term(), 1e-5f) << "not wound up to max_integral";
  EXPECT_EQ(0.f, rt->pid().derivative_term());
  tick(hub().ms + 1000);
  EXPECT_NEAR(0.11f, rt->pid().integral_term(), 1e-5f) << "6 degrees for 1 s at 0.01";
}

// The same after a fault: ten days without a reading, then one.
TEST_F(LongGaps, APidStartsAfreshAfterAFault) {
  ControllerRuntime *rt = this->start(integrating(), 20.f);
  tick(100000);
  tick(101000);
  ASSERT_NEAR(0.05f, rt->pid().integral_term(), 1e-5f);
  tick(111000);
  ASSERT_EQ(HubFault::SENSOR_STALE, rt->fault());

  hub().ms = 101000 + TEN_DAYS_MS;
  entities().room.publish_state(20.f);
  tick(hub().ms);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_NEAR(0.05f, rt->pid().integral_term(), 1e-5f) << "not wound up to max_integral";
  tick(hub().ms + 1000);
  EXPECT_NEAR(0.10f, rt->pid().integral_term(), 1e-5f);
}

// A temperature that fell before a pause gives the first pass after it no derivative: the window
// that averages it holds nothing from before.
TEST_F(LongGaps, APidAveragesNoDerivativeFromBeforeAPause) {
  ClimateConfig config = integrating();
  config.pid.ki = 0.f;
  config.pid.kd = 1.f;
  config.pid.derivative_samples = 4.f;
  config.safety.sensor_timeout_s = 60.f;
  ControllerRuntime *rt = this->start(config, 20.f);
  tick(100000);
  for (uint64_t t = 101000; t <= 102000; t += 1000) {
    entities().room.publish_state(20.f - static_cast<float>(t - 100000) / 2000.f);
    tick(t);
  }
  ASSERT_GT(rt->pid().derivative_term(), 0.f);

  this->mode(climate::CLIMATE_MODE_OFF);
  tick(103000);
  this->mode(climate::CLIMATE_MODE_HEAT);
  tick(104000);
  EXPECT_EQ(0.f, rt->pid().derivative_term());
  entities().room.publish_state(18.5f);
  tick(105000);
  EXPECT_NEAR(0.25f, rt->pid().derivative_term(), 1e-5f) << "0.5 a second averaged with the 0 after the pause";
}

// Nor does it average in an output from before the pause: here after a stale sensor.
TEST_F(LongGaps, APidAveragesNoOutputFromBeforeAPause) {
  ClimateConfig config = integrating();
  config.pid.kp = 0.1f;
  config.pid.ki = 0.f;
  config.pid.output_samples = 4.f;
  ControllerRuntime *rt = this->start(config, 20.f);
  tick(100000);
  tick(101000);
  ASSERT_NEAR(0.5f, rt->heat_duty(), 1e-5f);
  tick(111000);
  ASSERT_EQ(HubFault::SENSOR_STALE, rt->fault());

  entities().room.publish_state(24.f);
  tick(112000);
  ASSERT_EQ(HubFault::NONE, rt->fault());
  EXPECT_NEAR(0.1f, rt->heat_duty(), 1e-5f) << "a degree short at 0.1, not averaged with 0.5";
}

// relay_contested is no pause: the thermostat goes on, and its PID integrates every interval
// in full.
TEST_F(LongGaps, ARelayContestIsNoPause) {
  ClimateConfig config = integrating();
  config.setpoint = 20.f;
  config.pid.ki = 0.001f;
  config.safety.sensor_timeout_s = 3600.f;
  // Five degrees too warm: the relay is wanted open, and something else keeps closing it.
  ControllerRuntime *rt = this->start(config, 25.f);
  tick(100000);
  uint64_t t = 100000;
  for (uint32_t n = 1; n <= CONTEST_MOVES; n++) {
    entities().relay1.turn_on();
    tick(t += 1000);
    tick(t += PUT_BACK_FLOOR_MS);
    ASSERT_FALSE(entities().relay1.state) << "put back after move " << n;
  }
  ASSERT_EQ(HubFault::RELAY_CONTESTED, rt->fault());

  const float before = rt->pid().integral_term();
  tick(t + 30000);
  EXPECT_EQ(HubFault::RELAY_CONTESTED, rt->fault());
  EXPECT_NEAR(before - 0.15f, rt->pid().integral_term(), 1e-5f) << "-5 degrees for 30 s at 0.001";
}

// An interval is no pause: the pass at its end integrates over all of it.
TEST_F(LongGaps, APidIntegratesOverItsWholeInterval) {
  ClimateConfig config = integrating();
  config.pid.ki = 0.001f;
  config.update_interval_s = 60.f;
  config.safety.sensor_timeout_s = 300.f;
  ControllerRuntime *rt = this->start(config, 20.f);
  tick(100000);
  tick(130000);
  EXPECT_NEAR(0.f, rt->pid().integral_term(), 1e-5f) << "no pass before the interval is up";
  tick(160000);
  EXPECT_NEAR(0.3f, rt->pid().integral_term(), 1e-5f) << "5 degrees for 60 s at 0.001";
}

}  // namespace
}  // namespace esphome::climate_hub::testing
