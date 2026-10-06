// Gaps a device takes weeks to reach: a sensor silent past the point where a 32-bit millis()
// wraps, about 49.7 days. The hub's clock is the test's.
#include "common.h"

namespace esphome::climate_hub::testing {
namespace {

// 2^32 ms: where a 32-bit millis() count starts again from zero.
constexpr uint64_t MILLIS_WRAP = 1ull << 32;

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

// The relays and the PWM count in the low 32 bits of the clock, which wrap 3 s in here: the
// rhythm goes on through it, 5 s on in every 10.
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

}  // namespace
}  // namespace esphome::climate_hub::testing
