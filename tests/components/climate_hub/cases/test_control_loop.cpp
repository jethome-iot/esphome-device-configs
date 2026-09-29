// A running thermostat end to end: a sensor reading goes in, a relay state comes out. The hub's
// clock is the test's, so nothing here waits out a five-minute PWM period.
#include "common.h"

namespace esphome::climate_hub::testing {
namespace {

class ControlLoop : public HubTest {
 protected:
  ClimateConfig base(ControlKind kind) {
    ClimateConfig c;
    c.name = "Loop";
    c.kind = kind;
    c.sensor_id = "room";
    c.heat.relay_id = "relay_1";
    c.heat.period_s = 10.f;
    c.heat.min_on_s = 0.f;
    c.heat.min_off_s = 0.f;
    c.mode = HubMode::HEAT;
    c.update_interval_s = 1.f;
    c.safety.sensor_timeout_s = 86400.f;
    // Switching points at 20 and 21 for every bang-bang test below.
    c.setpoint = 20.5f;
    c.bang_bang.below = 0.5f;
    c.bang_bang.above = 0.5f;
    return c;
  }

  // Creates and starts `config` and gives it a first reading.
  ControllerRuntime *start(const ClimateConfig &config, float temperature) {
    this->id_ = this->create(config).id;
    entities().room.publish_state(temperature);
    return hub().runtime_of(this->id_);
  }

  // One pass of the hub's loop at `ms`.
  static void tick(uint32_t ms) {
    hub().ms = ms;
    hub().loop();
  }

  static void call(HubClimate *entity, climate::ClimateMode mode) {
    auto call = entity->make_call();
    call.set_mode(mode);
    call.perform();
  }

  static void target(HubClimate *entity, float value) {
    auto call = entity->make_call();
    call.set_target_temperature(value);
    call.perform();
  }

  // `config` driving relay_2 as its cooling relay, alone or beside its heating one.
  static ClimateConfig with_cooling(ClimateConfig config, bool keep_heat) {
    if (!keep_heat)
      config.heat.relay_id = "";
    config.cool.relay_id = "relay_2";
    config.cool.period_s = 10.f;
    config.cool.min_on_s = 0.f;
    config.cool.min_off_s = 0.f;
    config.mode = keep_heat ? HubMode::HEAT_COOL : HubMode::COOL;
    return config;
  }

  std::string id_;
};

}  // namespace

TEST_F(ControlLoop, BangBangClosesTheRelayWhenItIsTooCold) {
  ControllerRuntime *rt = this->start(this->base(ControlKind::BANG_BANG), 18.f);
  ASSERT_NE(nullptr, rt);

  tick(200000);
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_TRUE(rt->heat_relay_on());
  EXPECT_TRUE(entities().relay1.state);

  entities().room.publish_state(22.f);
  tick(202000);
  EXPECT_EQ(HubAction::IDLE, rt->action());
  EXPECT_FALSE(entities().relay1.state);
}

TEST_F(ControlLoop, BangBangLatchesBetweenTheSwitchingPoints) {
  ControllerRuntime *rt = this->start(this->base(ControlKind::BANG_BANG), 18.f);
  tick(200000);
  ASSERT_TRUE(rt->heat_relay_on());

  entities().room.publish_state(20.5f);
  tick(202000);
  EXPECT_TRUE(rt->heat_relay_on()) << "inside the band the previous action stands";
}

TEST_F(ControlLoop, PidRunsTheRelayOnADutyCycle) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.f;
  ControllerRuntime *rt = this->start(config, 20.f);
  ASSERT_NE(nullptr, rt);

  tick(200000);
  EXPECT_EQ(HubAction::HEATING, rt->action());
  // 5 degrees of error at kp 0.1 is half power.
  EXPECT_NEAR(0.5f, rt->heat_duty(), 1e-4f);
  EXPECT_TRUE(rt->heat_relay_on());

  // Past the on-fraction of a ten second period, the relay opens again.
  tick(206000);
  EXPECT_FALSE(rt->heat_relay_on());
}

TEST_F(ControlLoop, ReachingTheSetpointDropsTheDutyToZero) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.f;
  ControllerRuntime *rt = this->start(config, 20.f);

  tick(200000);
  ASSERT_GT(rt->heat_duty(), 0.f);

  entities().room.publish_state(26.f);
  tick(202000);
  EXPECT_FLOAT_EQ(0.f, rt->heat_duty());
  EXPECT_FALSE(rt->heat_relay_on());
  EXPECT_EQ(HubAction::IDLE, rt->action());
}

TEST_F(ControlLoop, ModeOffOpensTheRelayAndReportsOff) {
  ControllerRuntime *rt = this->start(this->base(ControlKind::BANG_BANG), 18.f);
  tick(200000);
  ASSERT_TRUE(rt->heat_relay_on());

  call(hub().entity_of(this->id_), climate::CLIMATE_MODE_OFF);
  tick(201000);
  EXPECT_EQ(HubAction::OFF, rt->action());
  EXPECT_FALSE(entities().relay1.state);
}

// A probe that stops reporting must not leave the heater latched on its last reading.
TEST_F(ControlLoop, AStaleSensorCutsTheOutput) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.sensor_timeout_s = 10.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(hub().ms);
  ASSERT_TRUE(rt->heat_relay_on());

  tick(hub().ms + 11000);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault());
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_EQ(HubAction::OFF, rt->action());
}

// Until the first sample the thermostat knows nothing, and knowing nothing it does not heat.
TEST_F(ControlLoop, NoReadingYetIsAStaleSensor) {
  this->id_ = this->create(this->base(ControlKind::BANG_BANG)).id;
  tick(200000);
  EXPECT_EQ(HubFault::SENSOR_STALE, hub().runtime(this->id_)->fault());
  EXPECT_FALSE(entities().relay1.state);
}

TEST_F(ControlLoop, OvertemperatureCutsTheOutput) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.max_temperature = 30.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(200000);
  ASSERT_TRUE(rt->heat_relay_on());

  entities().room.publish_state(35.f);
  tick(201000);
  EXPECT_EQ(HubFault::OVERTEMP, rt->fault());
  EXPECT_FALSE(entities().relay1.state);
}

// The cut-out holds a relay open that someone closes by hand meanwhile: the claim compares the
// switch, not what it believes it last did.
TEST_F(ControlLoop, AFaultReopensARelayClosedByHand) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.max_temperature = 30.f;
  ControllerRuntime *rt = this->start(config, 35.f);
  tick(200000);
  ASSERT_EQ(HubFault::OVERTEMP, rt->fault());
  ASSERT_FALSE(entities().relay1.state);

  entities().relay1.turn_on();
  tick(201000);
  EXPECT_FALSE(entities().relay1.state);
}

// Disabling is the one way a thermostat stops driving; it opens the relay on the way out.
TEST_F(ControlLoop, DisablingARunningThermostatOpensItsRelay) {
  this->start(this->base(ControlKind::BANG_BANG), 18.f);
  tick(200000);
  ASSERT_TRUE(entities().relay1.state);

  ASSERT_TRUE(hub().set_enabled(this->id_, false).ok);
  EXPECT_FALSE(entities().relay1.state);
  entities().room.publish_state(10.f);
  tick(201000);
  EXPECT_FALSE(entities().relay1.state) << "nothing drives it any more";
}

// The latch holds the last action between the switching points. Carried across a mode change
// it would keep the heater running while the entity says COOL.
TEST_F(ControlLoop, TheLatchDoesNotSurviveAModeChange) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.cool.relay_id = "relay_2";
  config.cool.period_s = 10.f;
  config.cool.min_on_s = 0.f;
  config.cool.min_off_s = 0.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  ASSERT_NE(nullptr, rt);

  tick(200000);
  ASSERT_EQ(HubAction::HEATING, rt->action());

  // Inside the band, so the latch is what holds HEATING.
  entities().room.publish_state(20.5f);
  tick(201000);
  ASSERT_TRUE(rt->heat_relay_on());

  call(hub().entity_of(this->id_), climate::CLIMATE_MODE_COOL);
  tick(202000);
  EXPECT_FALSE(rt->heat_relay_on()) << "the heater must not run in COOL";
  EXPECT_NE(HubAction::HEATING, rt->action());
}

// One NaN reading used to leave the integrator NaN for good: every clamp comparison against
// NaN is false, so nothing downstream could recover it.
TEST_F(ControlLoop, ANaNReadingIsIgnoredRatherThanStored) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.01f;
  ControllerRuntime *rt = this->start(config, 20.f);

  tick(200000);
  ASSERT_GT(rt->heat_duty(), 0.f);

  entities().room.publish_state(NAN);
  EXPECT_FLOAT_EQ(20.f, hub().entity_of(this->id_)->current_temperature);
  tick(202000);
  EXPECT_GT(rt->heat_duty(), 0.f) << "the last good reading still stands";
  EXPECT_EQ(HubFault::NONE, rt->fault());

  entities().room.publish_state(21.f);
  tick(204000);
  EXPECT_GT(rt->heat_duty(), 0.f) << "and the integrator is still a number";
  EXPECT_FALSE(std::isnan(rt->pid().integral_term()));
}

// update_interval_s may be a full hour; a mode or target change must not wait out the rest of it.
TEST_F(ControlLoop, ASetpointChangeIsActedOnAtTheNextTick) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.update_interval_s = 3600.f;
  config.setpoint = 19.f;
  config.pid.kp = 0.5f;
  config.pid.ki = 0.f;
  ControllerRuntime *rt = this->start(config, 20.f);

  tick(200000);
  ASSERT_FLOAT_EQ(0.f, rt->heat_duty()) << "already above the target";

  target(hub().entity_of(this->id_), 30.f);
  tick(201000);
  EXPECT_GT(rt->heat_duty(), 0.f) << "acted on now, not in an hour";
}

// climate::Climate unions target_temperature with the low/high pair, so writing the wrong half
// leaves clients reading a setpoint that was never set. Both algorithms are single-point here.
TEST_F(ControlLoop, BothAlgorithmsReportOneTarget) {
  this->start(this->base(ControlKind::BANG_BANG), 18.f);
  HubClimate *entity = hub().entity_of(this->id_);
  EXPECT_FALSE(entity->get_traits().has_feature_flags(climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE |
                                                      climate::CLIMATE_REQUIRES_TWO_POINT_TARGET_TEMPERATURE));
  EXPECT_FLOAT_EQ(20.5f, entity->target_temperature);
  EXPECT_FLOAT_EQ(20.f, hub().store().get(this->id_)->switch_low());
  EXPECT_FLOAT_EQ(21.f, hub().store().get(this->id_)->switch_high());

  ASSERT_TRUE(hub().remove(this->id_).ok);

  ClimateConfig pid = this->base(ControlKind::PID);
  pid.setpoint = 22.f;
  this->start(pid, 18.f);
  entity = hub().entity_of(this->id_);
  EXPECT_FALSE(entity->get_traits().has_feature_flags(climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE));
  EXPECT_FLOAT_EQ(22.f, entity->target_temperature);
}

// Moving the target moves both switching points with it: the band is stored, not the points.
TEST_F(ControlLoop, TheBandFollowsTheTarget) {
  ControllerRuntime *rt = this->start(this->base(ControlKind::BANG_BANG), 18.f);
  tick(200000);
  ASSERT_EQ(HubAction::HEATING, rt->action());

  target(hub().entity_of(this->id_), 17.f);
  tick(201000);
  EXPECT_EQ(HubAction::IDLE, rt->action()) << "18 is now above the 17.5 upper point";
  EXPECT_FALSE(rt->heat_relay_on());
}

// Whatever Home Assistant or the web server send, the document only holds a target inside its
// range, and the entity says so.
TEST_F(ControlLoop, ControlClampsTheTargetIntoTheRange) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.visual.min_temperature = 10.f;
  config.visual.max_temperature = 30.f;
  this->start(config, 18.f);
  HubClimate *entity = hub().entity_of(this->id_);

  target(entity, 99.f);
  EXPECT_FLOAT_EQ(30.f, hub().store().get(this->id_)->setpoint);
  EXPECT_FLOAT_EQ(30.f, entity->target_temperature);
  target(entity, -40.f);
  EXPECT_FLOAT_EQ(10.f, hub().store().get(this->id_)->setpoint);
  EXPECT_TRUE(hub().dirty(this->id_)) << "persisted, debounced";

  tick(hub().ms + 3000);
  ASSERT_FALSE(hub().dirty(this->id_));
  target(entity, NAN);
  target(entity, 10.f);
  EXPECT_FLOAT_EQ(10.f, entity->target_temperature);
  EXPECT_FALSE(hub().dirty(this->id_)) << "no number, then no change: nothing to write";
}

// Only the modes its relays can serve are offered, and nothing else is taken.
TEST_F(ControlLoop, ControlTakesOnlyTheModesItsRelaysServe) {
  this->start(this->base(ControlKind::BANG_BANG), 18.f);
  HubClimate *entity = hub().entity_of(this->id_);
  auto traits = entity->get_traits();
  EXPECT_TRUE(traits.supports_mode(climate::CLIMATE_MODE_OFF));
  EXPECT_TRUE(traits.supports_mode(climate::CLIMATE_MODE_HEAT));
  EXPECT_FALSE(traits.supports_mode(climate::CLIMATE_MODE_COOL));
  EXPECT_FALSE(traits.supports_mode(climate::CLIMATE_MODE_HEAT_COOL));

  call(entity, climate::CLIMATE_MODE_COOL);
  EXPECT_EQ(HubMode::HEAT, hub().store().get(this->id_)->mode);
  EXPECT_EQ(climate::CLIMATE_MODE_HEAT, entity->mode);
}

// A sample that changes nothing is not sent on: the API and the event stream see changes only.
TEST_F(ControlLoop, AnUnchangedReadingIsNotRepublished) {
  this->start(this->base(ControlKind::BANG_BANG), 18.f);
  HubClimate *entity = hub().entity_of(this->id_);
  // The entity outlives the test and keeps its callbacks, so the count must too.
  static int published = 0;
  entity->add_on_state_callback([](climate::Climate &) { published++; });
  const int before = published;

  entities().room.publish_state(18.f);
  entities().room.publish_state(18.f);
  EXPECT_EQ(before, published);
  entities().room.publish_state(18.5f);
  EXPECT_EQ(before + 1, published);
  EXPECT_FLOAT_EQ(18.5f, entity->current_temperature);
}

// Cooling is heating mirrored: on above the upper point, off below the lower one, latched between.
TEST_F(ControlLoop, BangBangCoolsAboveTheUpperPoint) {
  ControllerRuntime *rt = this->start(with_cooling(this->base(ControlKind::BANG_BANG), false), 23.f);
  HubClimate *entity = hub().entity_of(this->id_);
  EXPECT_EQ(climate::CLIMATE_MODE_COOL, entity->mode);

  tick(200000);
  EXPECT_EQ(HubAction::COOLING, rt->action());
  EXPECT_EQ(climate::CLIMATE_ACTION_COOLING, entity->action);
  EXPECT_TRUE(entities().relay2.state);
  EXPECT_FALSE(entities().relay1.state);

  entities().room.publish_state(20.5f);
  tick(201000);
  EXPECT_TRUE(entities().relay2.state) << "inside the band the previous action stands";

  entities().room.publish_state(19.f);
  tick(202000);
  EXPECT_EQ(HubAction::IDLE, rt->action());
  EXPECT_FALSE(entities().relay2.state);
}

// With both relays, heat and cool heats below the band, cools above it and idles inside it.
TEST_F(ControlLoop, HeatAndCoolUsesBothRelays) {
  ControllerRuntime *rt = this->start(with_cooling(this->base(ControlKind::BANG_BANG), true), 18.f);
  HubClimate *entity = hub().entity_of(this->id_);
  EXPECT_EQ(climate::CLIMATE_MODE_HEAT_COOL, entity->mode);
  auto traits = entity->get_traits();
  for (auto mode : {climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT, climate::CLIMATE_MODE_COOL,
                    climate::CLIMATE_MODE_HEAT_COOL})
    EXPECT_TRUE(traits.supports_mode(mode)) << mode;

  tick(200000);
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_TRUE(entities().relay1.state);
  EXPECT_FALSE(entities().relay2.state);

  entities().room.publish_state(20.5f);
  tick(201000);
  EXPECT_EQ(HubAction::IDLE, rt->action()) << "no latch when both directions are live";
  EXPECT_FALSE(entities().relay1.state);

  entities().room.publish_state(23.f);
  tick(202000);
  EXPECT_EQ(HubAction::COOLING, rt->action());
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_TRUE(entities().relay2.state);
}

// A negative PID output is cooling demand, at the same duty a positive one would heat.
TEST_F(ControlLoop, PidCoolsOnANegativeOutput) {
  ClimateConfig config = with_cooling(this->base(ControlKind::PID), false);
  config.setpoint = 20.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.f;
  ControllerRuntime *rt = this->start(config, 25.f);

  tick(200000);
  EXPECT_EQ(HubAction::COOLING, rt->action());
  EXPECT_NEAR(0.5f, rt->cool_duty(), 1e-4f);
  EXPECT_FLOAT_EQ(0.f, rt->heat_duty());
  EXPECT_TRUE(rt->cool_relay_on());
  EXPECT_TRUE(entities().relay2.state);
}

// Home Assistant turning a stopped-in-OFF thermostat back to HEAT, or a two-relay one to HEAT_COOL:
// the mode lands in the document and on the entity, and is written after the debounce.
TEST_F(ControlLoop, ControlTakesEveryModeItsRelaysServe) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.mode = HubMode::OFF;
  ControllerRuntime *rt = this->start(config, 18.f);
  HubClimate *entity = hub().entity_of(this->id_);

  call(entity, climate::CLIMATE_MODE_HEAT);
  EXPECT_EQ(HubMode::HEAT, hub().store().get(this->id_)->mode);
  EXPECT_EQ(climate::CLIMATE_MODE_HEAT, entity->mode);
  EXPECT_TRUE(hub().dirty(this->id_));
  tick(200000);
  EXPECT_EQ(HubAction::HEATING, rt->action());
  call(entity, climate::CLIMATE_MODE_HEAT_COOL);
  EXPECT_EQ(HubMode::HEAT, hub().store().get(this->id_)->mode) << "no cooling relay";

  ASSERT_TRUE(hub().remove(this->id_).ok);
  ClimateConfig both = with_cooling(this->base(ControlKind::BANG_BANG), true);
  both.mode = HubMode::HEAT;
  this->start(both, 18.f);
  entity = hub().entity_of(this->id_);
  call(entity, climate::CLIMATE_MODE_HEAT_COOL);
  EXPECT_EQ(HubMode::HEAT_COOL, hub().store().get(this->id_)->mode);
  EXPECT_EQ(climate::CLIMATE_MODE_HEAT_COOL, entity->mode);
}

// A cut-out lasts as long as its cause: the reading back under the limit, heating resumes.
TEST_F(ControlLoop, AFaultClearsWhenItsCauseDoes) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.max_temperature = 30.f;
  config.safety.sensor_timeout_s = 10.f;
  hub().ms = 200000;
  ControllerRuntime *rt = this->start(config, 35.f);
  tick(200000);
  ASSERT_EQ(HubFault::OVERTEMP, rt->fault());

  entities().room.publish_state(18.f);
  tick(201000);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_TRUE(entities().relay1.state);

  tick(212000);
  ASSERT_EQ(HubFault::SENSOR_STALE, rt->fault());
  entities().room.publish_state(18.f);
  tick(212500);
  EXPECT_EQ(HubFault::NONE, rt->fault()) << "a reading, even the same one, ends a stale fault";
  EXPECT_TRUE(entities().relay1.state);
}

// The editor's status card shows how old the last reading is.
TEST_F(ControlLoop, TheSampleAgeIsReported) {
  this->id_ = this->create(this->base(ControlKind::BANG_BANG)).id;
  ControllerRuntime *rt = hub().runtime_of(this->id_);
  EXPECT_FALSE(rt->has_sample());
  EXPECT_TRUE(std::isnan(rt->sensor_age_s(hub().ms)));

  entities().room.publish_state(18.f);
  EXPECT_TRUE(rt->has_sample());
  EXPECT_FLOAT_EQ(2.5f, rt->sensor_age_s(hub().ms + 2500));
  EXPECT_EQ(nullptr, hub().runtime("no-such-thermostat"));
  EXPECT_EQ(rt, hub().runtime(this->id_));
}

// A runtime with no document behind it, the state every slot starts in, answers nothing and
// drives nothing, whatever reaches it.
TEST(ControllerRuntimeAlone, AStoppedRuntimeIgnoresEverything) {
  HubClimate entity(&hub(), 200);
  ControllerRuntime rt(&entity);
  FakeSwitch relay;
  RelayClaim claim(&relay, "idle");

  rt.tick(1000);
  rt.on_sample(20.f, 1000);
  auto call = entity.make_call();
  call.set_target_temperature(25.f);
  EXPECT_FALSE(rt.control(call));
  rt.stop(1000);
  EXPECT_FALSE(rt.running());
  EXPECT_FALSE(rt.has_sample());
  EXPECT_EQ(0, relay.writes);

  // Running a document that is switched off: the hub never does, but it would not drive either.
  ClimateConfig config = draft("Idle");
  config.enabled = false;
  rt.start(&config, nullptr, &claim, nullptr, 1000);
  rt.tick(2000);
  EXPECT_EQ(0, relay.writes);
  EXPECT_EQ(HubAction::OFF, rt.action());
}

// The PID recomputes every update_interval_s, and holds its output in between.
TEST_F(ControlLoop, PidRecomputesOnlyEveryInterval) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.update_interval_s = 60.f;
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.f;
  ControllerRuntime *rt = this->start(config, 20.f);

  tick(200000);
  ASSERT_NEAR(0.5f, rt->heat_duty(), 1e-4f);
  entities().room.publish_state(23.f);
  tick(230000);
  EXPECT_NEAR(0.5f, rt->heat_duty(), 1e-4f) << "half an interval on: the output stands";
  tick(260000);
  EXPECT_NEAR(0.2f, rt->heat_duty(), 1e-4f) << "a full interval on: recomputed";
}

// A Save onto another probe reads that probe from then on, starting with what it says now.
TEST_F(ControlLoop, ASaveOntoAnotherProbeReadsIt) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  this->start(config, 19.f);
  entities().floor.publish_state(28.f);

  config.sensor_id = "floor";
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  HubClimate *entity = hub().entity_of(this->id_);
  EXPECT_FLOAT_EQ(28.f, entity->current_temperature);
  entities().room.publish_state(10.f);
  EXPECT_FLOAT_EQ(28.f, entity->current_temperature) << "the old probe is no longer listened to";
}

// A probe whose last word was "no reading" gives a starting thermostat nothing to go on.
TEST_F(ControlLoop, AProbeThatLastSaidNothingIsNoReading) {
  entities().room.publish_state(NAN);
  this->id_ = this->create(this->base(ControlKind::BANG_BANG)).id;
  ControllerRuntime *rt = hub().runtime_of(this->id_);
  EXPECT_FALSE(rt->has_sample());
  tick(200000);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault());
}

}  // namespace esphome::climate_hub::testing
