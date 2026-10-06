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

  // Every action `entity` publishes from here until the next watch(). Callbacks cannot be
  // removed and a slot outlives the test, so each entity is hooked once and records only while
  // it is the one watched.
  static std::vector<climate::ClimateAction> &watch(HubClimate *entity) {
    static std::vector<climate::ClimateAction> seen;
    static HubClimate *watched = nullptr;
    static std::set<HubClimate *> hooked;
    seen.clear();
    watched = entity;
    if (hooked.insert(entity).second) {
      entity->add_on_state_callback([entity](climate::Climate &c) {
        if (watched == entity)
          seen.push_back(c.action);
      });
    }
    return seen;
  }

  std::string id_;
};

using Actions = std::vector<climate::ClimateAction>;

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
  tick(210000);
  EXPECT_TRUE(rt->heat_relay_on());
  tick(216000);
  EXPECT_FALSE(rt->heat_relay_on());
  EXPECT_EQ(0u, hub().claim("relay_1")->moves()) << "its own switching is no move from elsewhere";
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

// Running in a mode other than off, a thermostat that neither heats nor cools is idle, whatever
// its law: off is for mode off, a fault, or a stopped thermostat.
TEST_F(ControlLoop, ARunningThermostatThatDoesNothingIsIdle) {
  for (ControlKind kind : {ControlKind::BANG_BANG, ControlKind::PID}) {
    SCOPED_TRACE(enums::control_kind_to_string(kind));
    ClimateConfig config = this->base(kind);
    config.pid.kp = 0.1f;
    config.pid.ki = 0.f;
    this->id_ = this->create(config).id;
    ControllerRuntime *rt = hub().runtime_of(this->id_);
    HubClimate *entity = hub().entity_of(this->id_);
    EXPECT_EQ(climate::CLIMATE_ACTION_IDLE, entity->action) << "as it starts";

    Actions &seen = watch(entity);
    // On the target, inside the band.
    entities().room.publish_state(20.5f);
    tick(200000);
    EXPECT_EQ(HubAction::IDLE, rt->action());
    EXPECT_EQ(climate::CLIMATE_ACTION_IDLE, entity->action);
    EXPECT_EQ(0, std::count(seen.begin(), seen.end(), climate::CLIMATE_ACTION_OFF));
    EXPECT_FALSE(entities().relay1.state);
    ASSERT_TRUE(hub().remove(this->id_).ok);
    reset_entities();
  }
}

// A mode change resets the latch; inside the band that leaves the thermostat idle, not off.
TEST_F(ControlLoop, AModeChangeInsideTheBandIdles) {
  ControllerRuntime *rt = this->start(with_cooling(this->base(ControlKind::BANG_BANG), true), 20.5f);
  HubClimate *entity = hub().entity_of(this->id_);
  tick(200000);
  ASSERT_EQ(HubAction::IDLE, rt->action());

  call(entity, climate::CLIMATE_MODE_HEAT);
  tick(201000);
  EXPECT_EQ(HubAction::IDLE, rt->action());
  EXPECT_EQ(climate::CLIMATE_ACTION_IDLE, entity->action);
  call(entity, climate::CLIMATE_MODE_COOL);
  tick(202000);
  EXPECT_EQ(HubAction::IDLE, rt->action());
}

// A mode from Home Assistant is published with the action it leads to, not the one of the mode
// it replaced: off at once for off, idle until the next pass decides otherwise.
TEST_F(ControlLoop, AModeChangeShowsItsActionAtOnce) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.mode = HubMode::OFF;
  ControllerRuntime *rt = this->start(config, 18.f);
  HubClimate *entity = hub().entity_of(this->id_);
  tick(200000);
  ASSERT_EQ(HubAction::OFF, rt->action());

  Actions &seen = watch(entity);
  call(entity, climate::CLIMATE_MODE_HEAT);
  EXPECT_EQ(Actions{climate::CLIMATE_ACTION_IDLE}, seen);
  tick(201000);
  EXPECT_EQ((Actions{climate::CLIMATE_ACTION_IDLE, climate::CLIMATE_ACTION_HEATING}), seen);

  seen.clear();
  call(entity, climate::CLIMATE_MODE_OFF);
  EXPECT_EQ(Actions{climate::CLIMATE_ACTION_OFF}, seen);
  tick(202000);
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_EQ(Actions{climate::CLIMATE_ACTION_OFF}, seen) << "the pass agrees";
}

// A Save republishes what the thermostat is doing, not "off" until its next pass.
TEST_F(ControlLoop, ASaveShowsWhatTheThermostatIsDoing) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.max_temperature = 30.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  HubClimate *entity = hub().entity_of(this->id_);
  tick(200000);
  ASSERT_EQ(HubAction::HEATING, rt->action());

  Actions &seen = watch(entity);
  config.update_interval_s = 2.f;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(201000);
  EXPECT_EQ(Actions{climate::CLIMATE_ACTION_HEATING}, seen) << "heating throughout";

  entities().room.publish_state(22.f);
  tick(203000);
  ASSERT_EQ(HubAction::IDLE, rt->action());
  seen.clear();
  config.update_interval_s = 3.f;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(204000);
  EXPECT_EQ(Actions{climate::CLIMATE_ACTION_IDLE}, seen) << "idle throughout";

  entities().room.publish_state(35.f);
  tick(205000);
  ASSERT_EQ(HubFault::OVERTEMP, rt->fault());
  seen.clear();
  LogCapture::instance().clear();
  config.update_interval_s = 4.f;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  EXPECT_EQ(HubFault::OVERTEMP, rt->fault()) << "the fault stands through the Save";
  tick(206000);
  EXPECT_EQ(Actions{climate::CLIMATE_ACTION_OFF}, seen) << "off throughout";
  EXPECT_FALSE(LogCapture::instance().has("overtemp")) << "and is not reported again";
}

// A PID in the off half of its PWM period is still heating, and a Save says so.
TEST_F(ControlLoop, ASaveOfAPidBetweenPulsesShowsHeating) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.f;
  config.update_interval_s = 60.f;
  ControllerRuntime *rt = this->start(config, 20.f);
  HubClimate *entity = hub().entity_of(this->id_);
  tick(200000);
  tick(206000);
  ASSERT_EQ(HubAction::HEATING, rt->action());
  ASSERT_FALSE(entities().relay1.state) << "past the half period the relay is open";

  Actions &seen = watch(entity);
  config.name = "Renamed";
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(207000);
  EXPECT_EQ(Actions{climate::CLIMATE_ACTION_HEATING}, seen);
}

// The same for cooling, in cool and in heat and cool.
TEST_F(ControlLoop, ASaveOfACoolingPidShowsCooling) {
  for (bool keep_heat : {false, true}) {
    SCOPED_TRACE(keep_heat ? "heat_cool" : "cool");
    ClimateConfig config = with_cooling(this->base(ControlKind::PID), keep_heat);
    config.setpoint = 20.f;
    config.pid.kp = 0.1f;
    config.pid.ki = 0.f;
    config.update_interval_s = 60.f;
    ControllerRuntime *rt = this->start(config, 25.f);
    HubClimate *entity = hub().entity_of(this->id_);
    tick(200000);
    tick(206000);
    ASSERT_EQ(HubAction::COOLING, rt->action());
    ASSERT_FALSE(entities().relay2.state) << "past the half period the relay is open";

    Actions &seen = watch(entity);
    config.update_interval_s = 30.f;
    ASSERT_TRUE(hub().update(this->id_, config).ok);
    tick(207000);
    EXPECT_EQ(Actions{climate::CLIMATE_ACTION_COOLING}, seen);
    ASSERT_TRUE(hub().remove(this->id_).ok);
    reset_entities();
    hub().ms = 100000;
  }
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

// Until the first sample the thermostat knows nothing, and knowing nothing it does not heat. It
// waits, idle and without a fault, for as long as the sensor may take to speak.
TEST_F(ControlLoop, NoReadingYetWaitsOutTheTimeoutWithTheRelaysOpen) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.sensor_timeout_s = 10.f;
  entities().relay1.turn_on();
  LogCapture::instance().clear();
  this->id_ = this->create(config).id;
  ControllerRuntime *rt = hub().runtime_of(this->id_);

  tick(100000);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_EQ(HubAction::IDLE, rt->action());
  EXPECT_EQ(climate::CLIMATE_ACTION_IDLE, hub().entity_of(this->id_)->action);
  EXPECT_FALSE(entities().relay1.state) << "no reading, no heating";
  tick(110000);
  EXPECT_EQ(HubFault::NONE, rt->fault()) << "the whole timeout, counted from the start";
  EXPECT_FALSE(LogCapture::instance().has("sensor_stale")) << "waiting is not worth a warning";

  tick(110001);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault());
  EXPECT_EQ(HubAction::OFF, rt->action());
  EXPECT_TRUE(LogCapture::instance().has("sensor_stale"));
  EXPECT_FALSE(entities().relay1.state);

  entities().room.publish_state(18.f);
  tick(111000);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_TRUE(entities().relay1.state);
}

// A Save is not a start: it does not give a silent sensor another full timeout. A Save onto
// another sensor does.
TEST_F(ControlLoop, TheWaitForAFirstReadingOutlastsASave) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.sensor_timeout_s = 10.f;
  this->id_ = this->create(config).id;
  ControllerRuntime *rt = hub().runtime_of(this->id_);

  hub().ms = 108000;
  config.update_interval_s = 2.f;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(110001);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault()) << "counted from 100 s, not from the Save";

  config.sensor_id = "floor";
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  EXPECT_EQ(HubFault::NONE, rt->fault()) << "a new sensor gets its own wait";
  tick(120001);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  tick(120002);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault());
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
  EXPECT_EQ(HubAction::IDLE, rt->action());
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

// An infinity is no reading either: -inf would latch the heater on below the band and +inf trip
// the cut-out, and Home Assistant would be shown either.
TEST_F(ControlLoop, AnInfiniteReadingIsIgnoredByTheHysteresis) {
  ControllerRuntime *rt = this->start(this->base(ControlKind::BANG_BANG), 22.f);
  HubClimate *entity = hub().entity_of(this->id_);
  // The entity outlives the test and keeps its callbacks: this one counts only while armed.
  static bool armed = false;
  static int infinities = 0;
  entity->add_on_state_callback([](climate::Climate &c) {
    if (armed && std::isinf(c.current_temperature))
      infinities++;
  });
  infinities = 0;
  armed = true;
  tick(200000);
  EXPECT_EQ(HubAction::IDLE, rt->action());

  entities().room.publish_state(-INFINITY);
  EXPECT_FLOAT_EQ(22.f, entity->current_temperature);
  tick(202000);
  EXPECT_EQ(HubAction::IDLE, rt->action()) << "-inf is not too cold";
  EXPECT_FALSE(entities().relay1.state);

  entities().room.publish_state(18.f);
  tick(204000);
  EXPECT_EQ(HubAction::HEATING, rt->action());
  entities().room.publish_state(INFINITY);
  EXPECT_FLOAT_EQ(18.f, entity->current_temperature);
  tick(206000);
  EXPECT_EQ(HubAction::HEATING, rt->action()) << "+inf is neither too hot nor over the cut-out";
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_TRUE(entities().relay1.state);
  armed = false;
  EXPECT_EQ(0, infinities) << "no infinity was published";
}

// Through the PID, -inf would wind the integral to its limit and fill the derivative window with
// infinities, and +inf trip the cut-out: each pass runs on the last finite reading instead.
TEST_F(ControlLoop, AnInfiniteReadingIsIgnoredByThePid) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.01f;
  config.pid.kd = 1.f;
  config.pid.derivative_samples = 4;
  ControllerRuntime *rt = this->start(config, 20.f);
  HubClimate *entity = hub().entity_of(this->id_);
  tick(200000);

  entities().room.publish_state(-INFINITY);
  EXPECT_FLOAT_EQ(20.f, entity->current_temperature);
  tick(202000);
  // Two seconds of a 5 degree error at ki 0.01, and an error that has not moved.
  EXPECT_NEAR(0.1f, rt->pid().integral_term(), 1e-5f) << "not wound up to max_integral";
  EXPECT_FLOAT_EQ(0.f, rt->pid().derivative_term());
  EXPECT_NEAR(0.6f, rt->heat_duty(), 1e-4f);

  entities().room.publish_state(INFINITY);
  EXPECT_FLOAT_EQ(20.f, entity->current_temperature);
  tick(204000);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_NEAR(0.2f, rt->pid().integral_term(), 1e-5f);
  EXPECT_FLOAT_EQ(0.f, rt->pid().derivative_term());

  entities().room.publish_state(21.f);
  tick(206000);
  EXPECT_TRUE(std::isfinite(rt->pid().derivative_term())) << "no infinity sits in the window";
  EXPECT_TRUE(std::isfinite(rt->pid().integral_term()));
  EXPECT_GT(rt->heat_duty(), 0.f);
}

// A probe that sends nothing but infinities is as good as silent: stale once its timeout has
// run from the last finite reading, still showing that reading.
TEST_F(ControlLoop, AProbeSendingOnlyInfinitiesGoesStale) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.sensor_timeout_s = 10.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(100000);
  EXPECT_TRUE(entities().relay1.state);

  for (uint32_t ms = 101000; ms <= 110000; ms += 1000) {
    hub().ms = ms;
    entities().room.publish_state(ms % 2000 == 0 ? INFINITY : -INFINITY);
    tick(ms);
    EXPECT_EQ(HubFault::NONE, rt->fault()) << ms;
  }
  hub().ms = 110001;
  entities().room.publish_state(-INFINITY);
  tick(110001);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault());
  EXPECT_EQ(HubAction::OFF, rt->action());
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_FLOAT_EQ(18.f, hub().entity_of(this->id_)->current_temperature) << "the last finite reading";
  EXPECT_NEAR(10.001f, rt->sensor_age_s(110001), 1e-4f);
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

// With both relays wired, the mode alone says which may run: a demand the other way, a PID
// output of the other sign or a reading past the other switching point, leaves its relay open.
TEST_F(ControlLoop, ASingleDirectionModeNeverDrivesTheOtherRelay) {
  struct Case {
    const char *what;
    ControlKind kind;
    HubMode mode;
    float reading;
  };
  for (const Case &c : {Case{"pid in heat above the target", ControlKind::PID, HubMode::HEAT, 25.f},
                        Case{"pid in cool below the target", ControlKind::PID, HubMode::COOL, 16.f},
                        Case{"bang-bang in cool below the band", ControlKind::BANG_BANG, HubMode::COOL, 18.f},
                        Case{"bang-bang in heat above the band", ControlKind::BANG_BANG, HubMode::HEAT, 23.f}}) {
    SCOPED_TRACE(c.what);
    ClimateConfig config = with_cooling(this->base(c.kind), true);
    config.mode = c.mode;
    config.pid.kp = 0.1f;
    config.pid.ki = 0.f;
    ControllerRuntime *rt = this->start(config, c.reading);
    tick(200000);
    tick(201000);
    EXPECT_EQ(HubAction::IDLE, rt->action());
    EXPECT_FLOAT_EQ(0.f, rt->heat_duty());
    EXPECT_FLOAT_EQ(0.f, rt->cool_duty());
    EXPECT_FALSE(entities().relay1.state);
    EXPECT_FALSE(entities().relay2.state);
    EXPECT_EQ(0, entities().relay1.writes + entities().relay2.writes) << "neither relay ever moved";
    ASSERT_TRUE(hub().remove(this->id_).ok);
    reset_entities();
    hub().ms = 100000;
  }
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

// Home Assistant sends what a user set, so picking the mode the thermostat already has sends it
// again. That is no change: nothing to write, and the latch holds inside the band.
TEST_F(ControlLoop, ACallWithTheSameModeMarksNothingDirty) {
  ControllerRuntime *rt = this->start(this->base(ControlKind::BANG_BANG), 18.f);
  HubClimate *entity = hub().entity_of(this->id_);
  tick(200000);
  entities().room.publish_state(20.5f);
  tick(201000);
  ASSERT_EQ(HubAction::HEATING, rt->action()) << "latched inside the band";

  call(entity, climate::CLIMATE_MODE_HEAT);
  EXPECT_FALSE(hub().dirty(this->id_));
  tick(202000);
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_TRUE(entities().relay1.state);
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

// The cut-out zeroed the duties; with an hour's interval, waiting for the next pass would leave
// the thermostat off, without a fault, for the rest of the hour.
TEST_F(ControlLoop, AClearedFaultIsActedOnAtTheNextTick) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.update_interval_s = 3600.f;
  config.safety.max_temperature = 30.f;
  config.safety.sensor_timeout_s = 10.f;
  hub().ms = 200000;
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(200000);
  ASSERT_TRUE(rt->heat_relay_on());

  entities().room.publish_state(35.f);
  tick(201000);
  ASSERT_EQ(HubFault::OVERTEMP, rt->fault());
  entities().room.publish_state(18.f);
  tick(202000);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_EQ(HubAction::HEATING, rt->action()) << "after an over-temperature cut-out";
  EXPECT_TRUE(entities().relay1.state);

  tick(213000);
  ASSERT_EQ(HubFault::SENSOR_STALE, rt->fault());
  entities().room.publish_state(18.f);
  tick(214000);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_EQ(HubAction::HEATING, rt->action()) << "after a stale sensor";
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
  rt.start(&config, nullptr, &claim, nullptr, 2000);
  rt.tick(2000);
  EXPECT_EQ(0, relay.writes);
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
  EXPECT_EQ(HubFault::NONE, rt->fault()) << "waiting, inside the timeout";
  EXPECT_EQ(HubAction::IDLE, rt->action());
  EXPECT_FALSE(entities().relay1.state);
}

// Nor does one whose last word was an infinity: the entity shows no temperature rather than it.
TEST_F(ControlLoop, AProbeThatLastSaidInfinityIsNoReading) {
  for (float value : {-INFINITY, INFINITY}) {
    hub().ms = 100000;
    entities().room.publish_state(value);
    this->id_ = this->create(this->base(ControlKind::BANG_BANG)).id;
    ControllerRuntime *rt = hub().runtime_of(this->id_);
    ASSERT_NE(nullptr, rt);
    EXPECT_FALSE(rt->has_sample()) << value;
    EXPECT_TRUE(std::isnan(hub().entity_of(this->id_)->current_temperature)) << value;
    tick(200000);
    EXPECT_EQ(HubFault::NONE, rt->fault()) << value;
    EXPECT_EQ(HubAction::IDLE, rt->action()) << value;
    EXPECT_FALSE(entities().relay1.state) << value;
    ASSERT_TRUE(hub().remove(this->id_).ok);
  }
}

// A sensor state that arrived before any thermostat listened: shown if it is a finite number,
// never acted on.
TEST_F(ControlLoop, AStateTheHubNeverHeardIsShownNotActedOn) {
  for (float value : {18.f, -INFINITY}) {
    hub().ms = 100000;
    // Set without a publish, so no subscription hears it.
    entities().room.state = value;
    entities().room.set_has_state(true);
    this->id_ = this->create(this->base(ControlKind::BANG_BANG)).id;
    ControllerRuntime *rt = hub().runtime_of(this->id_);
    ASSERT_NE(nullptr, rt);
    const float shown = hub().entity_of(this->id_)->current_temperature;
    if (std::isfinite(value)) {
      EXPECT_FLOAT_EQ(value, shown);
    } else {
      EXPECT_TRUE(std::isnan(shown)) << value;
    }
    EXPECT_FALSE(rt->has_sample()) << value;
    tick(200000);
    EXPECT_EQ(HubAction::IDLE, rt->action()) << value;
    EXPECT_FALSE(entities().relay1.state) << "18 is below the band, but not a reading";
    ASSERT_TRUE(hub().remove(this->id_).ok);
  }
}

// Traits carry two steps: the target's, the thermostat's own, and the room's, which Home
// Assistant would otherwise round to half a degree.
TEST_F(ControlLoop, TheRoomTemperatureIsShownToATenthWhateverTheTargetStep) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.visual.step = 1.f;
  this->start(config, 18.f);
  auto traits = hub().entity_of(this->id_)->get_traits();
  EXPECT_FLOAT_EQ(1.f, traits.get_visual_target_temperature_step());
  EXPECT_FLOAT_EQ(0.1f, traits.get_visual_current_temperature_step());
}

// Stopped and started again before min_off ran out: the relay's dwell outlives the claim.
TEST_F(ControlLoop, ADisableAndEnableInsideMinOffKeepsTheRelayOpen) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.heat.min_off_s = 60.f;
  this->start(config, 18.f);
  tick(200000);
  ASSERT_TRUE(entities().relay1.state);

  ASSERT_TRUE(hub().set_enabled(this->id_, false).ok);
  ASSERT_FALSE(entities().relay1.state);
  hub().ms = 210000;
  ASSERT_TRUE(hub().set_enabled(this->id_, true).ok);
  ControllerRuntime *rt = hub().runtime_of(this->id_);
  tick(210000);
  EXPECT_EQ(HubFault::NONE, rt->fault()) << "the reading the hub saw still counts, at its age";
  EXPECT_FALSE(entities().relay1.state) << "opened at 200 s, so it stays open until 260 s";
  tick(259999);
  EXPECT_FALSE(entities().relay1.state);
  tick(260000);
  EXPECT_TRUE(entities().relay1.state);
}

// Held open while there is no reading, the relay never moved: the wait starts no min_off of its
// own, and the one from the boot ran out long ago.
TEST_F(ControlLoop, TheFirstReadingClosesARelayTheWaitNeverMoved) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.heat.min_off_s = 60.f;
  this->id_ = this->create(config).id;
  tick(100000);
  tick(101000);
  ASSERT_FALSE(entities().relay1.state);
  EXPECT_EQ(0, entities().relay1.writes) << "nothing to open";

  entities().room.publish_state(18.f);
  tick(102000);
  EXPECT_EQ(HubAction::HEATING, hub().runtime_of(this->id_)->action());
  EXPECT_TRUE(entities().relay1.state);
}

// An idle first pass keeps an open relay open: no write, and no min_off from it.
TEST_F(ControlLoop, TheFirstIdlePassMovesNothing) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.heat.min_off_s = 60.f;
  this->start(config, 20.5f);
  tick(100000);
  ASSERT_FALSE(entities().relay1.state);
  EXPECT_EQ(0, entities().relay1.writes) << "nothing to open";

  entities().room.publish_state(18.f);
  tick(105000);
  EXPECT_TRUE(entities().relay1.state) << "min_off ran out 60 s after the boot";
}

// Nor does a thermostat that stopped before it ever moved the relay hold back the next one on it.
TEST_F(ControlLoop, AStopThatNeverMovedTheRelayLeavesNoDwellBehind) {
  ClimateConfig waiting = this->base(ControlKind::BANG_BANG);
  waiting.name = "Waiting";
  this->create(waiting);
  tick(100000);
  ASSERT_TRUE(hub().set_enabled("waiting", false).ok);

  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.heat.min_off_s = 60.f;
  this->start(config, 18.f);
  tick(101000);
  EXPECT_TRUE(entities().relay1.state) << "open since the boot, more than 60 s ago";
}

// The reset or an ALWAYS_OFF restore opened the relay, so min_off runs from the boot: a
// brown-out or a boot loop does not short-cycle a compressor.
TEST_F(ControlLoop, MinOffHoldsAcrossAReboot) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.heat.min_off_s = 300.f;
  this->start(config, 18.f);
  tick(400000);
  ASSERT_TRUE(entities().relay1.state);

  this->reboot();
  ASSERT_FALSE(entities().relay1.state);
  ASSERT_TRUE(hub().is_running(this->id_));
  entities().room.publish_state(18.f);
  tick(102000);
  EXPECT_FALSE(entities().relay1.state) << "the device's clock starts at 0, this test's boot at 100 s";
  tick(299999);
  EXPECT_FALSE(entities().relay1.state);
  tick(300000);
  EXPECT_TRUE(entities().relay1.state);
}

// Summer taking over a boiler Winter has closed wants it closed too: it changes hands as it is.
TEST_F(ControlLoop, ATakeOverOfAClosedRelayKeepsItClosed) {
  ClimateConfig winter = this->base(ControlKind::BANG_BANG);
  winter.name = "Winter";
  winter.heat.min_off_s = 60.f;
  this->start(winter, 18.f);
  tick(200000);
  ASSERT_TRUE(entities().relay1.state);
  ClimateConfig summer = winter;
  summer.name = "Summer";
  summer.enabled = false;
  this->create(summer);
  const int writes = entities().relay1.writes;

  ASSERT_TRUE(hub().set_enabled("summer", true, true).ok);
  EXPECT_EQ("summer", hub().claimed_by("relay_1"));
  EXPECT_TRUE(entities().relay1.state);
  tick(201000);
  EXPECT_EQ(HubAction::HEATING, hub().runtime_of("summer")->action());
  EXPECT_TRUE(entities().relay1.state);
  EXPECT_EQ(writes, entities().relay1.writes) << "the relay never moved";
}

// Only what the taker names changes hands, as it is; the holder's other relay opens and is let go.
TEST_F(ControlLoop, ATakeOverOpensTheHoldersOtherRelay) {
  ClimateConfig winter = with_cooling(this->base(ControlKind::BANG_BANG), true);
  winter.name = "Winter";
  this->start(winter, 23.f);
  tick(200000);
  ASSERT_TRUE(entities().relay2.state);
  ClimateConfig summer = winter;
  summer.name = "Summer";
  summer.heat.relay_id = "relay_3";
  summer.enabled = false;
  this->create(summer);
  const int writes = entities().relay2.writes;

  ASSERT_TRUE(hub().set_enabled("summer", true, true).ok);
  EXPECT_EQ("", hub().claimed_by("relay_1"));
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_EQ("summer", hub().claimed_by("relay_2"));
  tick(201000);
  EXPECT_EQ(HubAction::COOLING, hub().runtime_of("summer")->action());
  EXPECT_TRUE(entities().relay2.state);
  EXPECT_EQ(writes, entities().relay2.writes) << "the cooling relay never moved";
  EXPECT_FALSE(entities().relay3.state);
}

// A relay from each of two holders: both change hands in the one take-over, as they are.
TEST_F(ControlLoop, ATakeOverFromTwoHoldersTakesBothRelays) {
  ClimateConfig heater = this->base(ControlKind::BANG_BANG);
  heater.name = "Heater";
  this->start(heater, 18.f);
  ClimateConfig cooler = with_cooling(this->base(ControlKind::BANG_BANG), false);
  cooler.name = "Cooler";
  this->create(cooler);
  tick(200000);
  ASSERT_TRUE(entities().relay1.state);
  ClimateConfig both = with_cooling(this->base(ControlKind::BANG_BANG), true);
  both.name = "Both";
  both.enabled = false;
  this->create(both);
  const int writes = entities().relay1.writes;

  ASSERT_TRUE(hub().set_enabled("both", true, true).ok);
  EXPECT_EQ("both", hub().claimed_by("relay_1"));
  EXPECT_EQ("both", hub().claimed_by("relay_2"));
  EXPECT_FALSE(hub().is_running("heater"));
  EXPECT_FALSE(hub().is_running("cooler"));
  tick(201000);
  EXPECT_EQ(HubAction::HEATING, hub().runtime_of("both")->action());
  EXPECT_TRUE(entities().relay1.state);
  EXPECT_EQ(writes, entities().relay1.writes) << "the heating relay never moved";
}

// A Save onto a relay another thermostat opened a moment ago waits out that relay's min_off.
TEST_F(ControlLoop, ASaveOntoAnotherRelayHonoursThatRelaysLastSwitching) {
  ClimateConfig other = this->base(ControlKind::BANG_BANG);
  other.name = "Other";
  other.heat.relay_id = "relay_2";
  this->start(other, 18.f);
  tick(200000);
  ASSERT_TRUE(entities().relay2.state);
  ASSERT_TRUE(hub().set_enabled(this->id_, false).ok);
  ASSERT_FALSE(entities().relay2.state);

  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.heat.min_off_s = 60.f;
  this->start(config, 18.f);
  tick(210000);
  ASSERT_TRUE(entities().relay1.state);
  config.heat.relay_id = "relay_2";
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(220000);
  EXPECT_FALSE(entities().relay2.state) << "relay 2 opened at 200 s";
  tick(260000);
  EXPECT_TRUE(entities().relay2.state);
}

// A Save while the room sits inside the band carries on heating, as the relay was.
TEST_F(ControlLoop, ASaveInsideTheBandKeepsHeating) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(200000);
  entities().room.publish_state(20.5f);
  tick(201000);
  ASSERT_TRUE(entities().relay1.state);

  config.update_interval_s = 2.f;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(202000);
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_TRUE(entities().relay1.state) << "stops above 21, not now";
}

// The latch let the relay go and min_on still holds it closed: a Save inside the band keeps
// the latch idle rather than reading the closed relay as heating, so it opens when min_on ends.
TEST_F(ControlLoop, ASaveInsideTheBandKeepsTheLatchNotTheRelay) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.heat.min_on_s = 60.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(200000);
  ASSERT_TRUE(entities().relay1.state);
  entities().room.publish_state(21.5f);
  tick(201000);
  ASSERT_EQ(RelayWait::MIN_ON, rt->heat_relay_wait()) << "the latch let it go";
  ASSERT_EQ(HubAction::HEATING, rt->action()) << "as the relay does";
  entities().room.publish_state(20.5f);
  tick(202000);
  ASSERT_TRUE(entities().relay1.state) << "min_on holds it until 260 s";

  config.update_interval_s = 2.f;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(203000);
  EXPECT_EQ(RelayWait::MIN_ON, rt->heat_relay_wait()) << "still let go";
  tick(260000);
  EXPECT_FALSE(entities().relay1.state) << "not heated on up to 21";
  EXPECT_EQ(HubAction::IDLE, rt->action());
}

// A Save into a mode that does not drive the latched direction drops it, as control() does.
TEST_F(ControlLoop, ASaveIntoAnotherModeDropsTheLatch) {
  ClimateConfig config = with_cooling(this->base(ControlKind::BANG_BANG), true);
  config.mode = HubMode::HEAT;
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(200000);
  entities().room.publish_state(20.5f);
  tick(201000);
  ASSERT_EQ(HubAction::HEATING, rt->action());

  config.mode = HubMode::COOL;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  EXPECT_EQ(HubAction::HEATING, rt->action()) << "the relay is closed until the next pass";
  tick(202000);
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_FALSE(entities().relay2.state);
  EXPECT_EQ(HubAction::IDLE, rt->action());
}

// The slow PWM keeps its rhythm across a Save; only a new period starts a new one.
TEST_F(ControlLoop, ASaveKeepsThePwmPhaseUnlessThePeriodChanges) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.f;
  this->start(config, 20.f);
  tick(200000);
  ASSERT_TRUE(entities().relay1.state) << "half of a ten second period, from 200 s";

  config.update_interval_s = 2.f;
  hub().ms = 203000;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(206000);
  EXPECT_FALSE(entities().relay1.state) << "6 s into the period that began at 200 s";

  config.heat.period_s = 20.f;
  hub().ms = 207000;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(208000);
  EXPECT_TRUE(entities().relay1.state) << "a new period begins at the next pass";
}

// A Save that keeps the law and the sensor keeps what the PID has learnt: a new name or a nudged
// gain must not dip the room. The next pass integrates from the last one, with the new gains.
TEST_F(ControlLoop, ASaveKeepsThePidState) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.01f;
  config.pid.starting_integral_term = 0.02f;
  ControllerRuntime *rt = this->start(config, 20.f);
  tick(200000);
  tick(201000);
  tick(202000);
  // 0.02 to start, then 5 degrees for 2 s at 0.01.
  ASSERT_NEAR(0.12f, rt->pid().integral_term(), 1e-5f);

  config.name = "Renamed";
  config.pid.kp = 0.12f;
  hub().ms = 202500;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  EXPECT_NEAR(0.12f, rt->pid().integral_term(), 1e-5f) << "not back to the starting term";
  tick(203000);
  EXPECT_NEAR(0.17f, rt->pid().integral_term(), 1e-5f) << "1 s on from the pass at 202 s";
  EXPECT_NEAR(5.f * 0.12f + 0.17f, rt->heat_duty(), 1e-4f) << "at the new kp";
}

// Narrower integral limits take the kept integral in with them at once, not after it has moved.
TEST_F(ControlLoop, ASaveClampsTheKeptIntegral) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.f;
  config.pid.ki = 0.1f;
  ControllerRuntime *rt = this->start(config, 20.f);
  tick(200000);
  tick(201000);
  ASSERT_NEAR(0.5f, rt->pid().integral_term(), 1e-5f);

  config.pid.max_integral = 0.3f;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  EXPECT_NEAR(0.3f, rt->pid().integral_term(), 1e-5f);
  entities().room.publish_state(26.f);
  tick(202000);
  EXPECT_NEAR(0.2f, rt->pid().integral_term(), 1e-5f) << "0.3 less 1 degree for 1 s at 0.1";
}

// What a PID learnt belongs to its law and its sensor: another of either, or a start from
// stopped, begins again from the starting integral.
TEST_F(ControlLoop, AnotherLawOrSensorOrAStartResetsThePid) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.f;
  config.pid.ki = 0.01f;
  config.pid.starting_integral_term = 0.3f;
  ControllerRuntime *rt = this->start(config, 20.f);
  tick(200000);
  tick(201000);
  ASSERT_NEAR(0.35f, rt->pid().integral_term(), 1e-5f);

  config.sensor_id = "floor";
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  entities().floor.publish_state(20.f);
  tick(202000);
  EXPECT_NEAR(0.3f, rt->pid().integral_term(), 1e-5f) << "another sensor";
  tick(203000);
  ASSERT_NEAR(0.35f, rt->pid().integral_term(), 1e-5f);

  config.kind = ControlKind::BANG_BANG;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(204000);
  config.kind = ControlKind::PID;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(205000);
  EXPECT_NEAR(0.3f, rt->pid().integral_term(), 1e-5f) << "back from the hysteresis";
  tick(206000);
  ASSERT_NEAR(0.35f, rt->pid().integral_term(), 1e-5f);

  ASSERT_TRUE(hub().set_enabled(this->id_, false).ok);
  ASSERT_TRUE(hub().set_enabled(this->id_, true).ok);
  rt = hub().runtime_of(this->id_);
  tick(207000);
  EXPECT_NEAR(0.3f, rt->pid().integral_term(), 1e-5f) << "started from stopped";
}

// A probe that last spoke a minute ago, beyond its timeout: its value is shown, not acted on.
TEST_F(ControlLoop, AProbeSilentSinceBeforeTheStartIsStale) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.sensor_timeout_s = 10.f;
  // The hub hears the probe only once a thermostat on it has run.
  ClimateConfig listener = config;
  listener.name = "Listener";
  listener.heat.relay_id = "relay_2";
  this->create(listener);
  entities().room.publish_state(18.f);
  ASSERT_TRUE(hub().remove("listener").ok);
  hub().ms += 60000;
  this->id_ = this->create(config).id;
  ControllerRuntime *rt = hub().runtime_of(this->id_);
  EXPECT_FLOAT_EQ(18.f, hub().entity_of(this->id_)->current_temperature);

  tick(hub().ms);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault());
  EXPECT_FALSE(entities().relay1.state);
  entities().room.publish_state(18.f);
  tick(hub().ms + 1000);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_TRUE(entities().relay1.state);
}

// A reading from before the start is acted on, and the timeout runs from it, not from the start.
TEST_F(ControlLoop, AReadingFromBeforeTheStartKeepsItsAge) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.sensor_timeout_s = 10.f;
  ClimateConfig listener = config;
  listener.name = "Listener";
  listener.heat.relay_id = "relay_2";
  this->create(listener);
  entities().room.publish_state(18.f);
  ASSERT_TRUE(hub().remove("listener").ok);
  hub().ms = 105000;
  this->id_ = this->create(config).id;
  ControllerRuntime *rt = hub().runtime_of(this->id_);

  tick(109000);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_TRUE(entities().relay1.state) << "a reading 9 s old is inside the timeout";
  tick(110001);
  EXPECT_EQ(HubFault::SENSOR_STALE, rt->fault()) << "10 s after the reading, 5 s after the start";
  EXPECT_FALSE(entities().relay1.state);
}

// --- Relays moved from elsewhere ---

// Home Assistant or the web server turning a held relay off before every pass: back at once the
// first time, then once per PUT_BACK_FLOOR_MS at most, since min_off is 0. The action follows
// the relay, and a put-back is no wait of the thermostat's own.
TEST_F(ControlLoop, AWriterThatKeepsAtItGetsOneSwitchPerFloor) {
  ControllerRuntime *rt = this->start(this->base(ControlKind::BANG_BANG), 18.f);
  tick(200000);
  ASSERT_TRUE(entities().relay1.state);

  std::vector<uint32_t> put_backs;
  for (uint32_t t = 201000; t <= 260000; t += 1000) {
    if (entities().relay1.state)
      entities().relay1.turn_off();
    tick(t);
    if (entities().relay1.state)
      put_backs.push_back(t);
    EXPECT_EQ(entities().relay1.state ? HubAction::HEATING : HubAction::IDLE, rt->action()) << "at " << t;
    EXPECT_EQ(RelayWait::NONE, rt->heat_relay_wait()) << "at " << t;
  }
  EXPECT_EQ((std::vector<uint32_t>{201000, 212000, 223000, 234000, 245000, 256000}), put_backs);
}

// Five moves raise relay_contested, which only reports: the thermostat goes on heating, putting
// the relay back and following the room. Ten minutes after the last put-back it clears.
TEST_F(ControlLoop, RelayContestedReportsAndClearsAfterTenQuietMinutes) {
  ControllerRuntime *rt = this->start(this->base(ControlKind::BANG_BANG), 18.f);
  HubClimate *entity = hub().entity_of(this->id_);
  tick(200000);
  LogCapture::instance().clear();
  uint32_t t = 200000;
  for (uint32_t n = 1; n <= CONTEST_MOVES; n++) {
    entities().relay1.turn_off();
    tick(t += 1000);
    EXPECT_EQ(HubFault::NONE, rt->fault()) << "move " << n;
    tick(t += PUT_BACK_FLOOR_MS);
    ASSERT_TRUE(entities().relay1.state) << "put back after move " << n;
  }
  EXPECT_EQ(HubFault::RELAY_CONTESTED, rt->fault());
  EXPECT_TRUE(LogCapture::instance().has("relay_contested"));
  EXPECT_TRUE(LogCapture::instance().has("moved from elsewhere (5)"));
  EXPECT_EQ(HubAction::HEATING, rt->action()) << "it goes on heating";
  EXPECT_EQ(climate::CLIMATE_ACTION_HEATING, entity->action);

  entities().room.publish_state(22.f);
  tick(t + 1000);
  EXPECT_EQ(HubAction::IDLE, rt->action()) << "and following the room";
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_EQ(HubFault::RELAY_CONTESTED, rt->fault());

  tick(t + CONTEST_QUIET_MS - 1);
  EXPECT_EQ(HubFault::RELAY_CONTESTED, rt->fault());
  LogCapture::instance().clear();
  tick(t + CONTEST_QUIET_MS);
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_TRUE(LogCapture::instance().has("fault cleared"));
}

// The relay a contest is about may be the cooling one.
TEST_F(ControlLoop, AContestedCoolingRelayIsReported) {
  ControllerRuntime *rt = this->start(with_cooling(this->base(ControlKind::BANG_BANG), true), 23.f);
  tick(200000);
  ASSERT_TRUE(entities().relay2.state);
  uint32_t t = 200000;
  for (uint32_t n = 1; n <= CONTEST_MOVES; n++) {
    entities().relay2.turn_off();
    tick(t += 1000);
    tick(t += PUT_BACK_FLOOR_MS);
  }
  EXPECT_EQ(HubFault::RELAY_CONTESTED, rt->fault());
  EXPECT_EQ(HubAction::COOLING, rt->action());
}

// A cut-out beats relay_contested: the relay opens, a close from elsewhere is undone on every
// pass, and when the cut-out clears, which the log says, control resumes at once, with an
// hour's interval too.
TEST_F(ControlLoop, ACutOutWinsOverRelayContested) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.update_interval_s = 3600.f;
  config.safety.max_temperature = 30.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(200000);
  uint32_t t = 200000;
  for (uint32_t n = 1; n <= CONTEST_MOVES; n++) {
    entities().relay1.turn_off();
    tick(t += 1000);
    tick(t += PUT_BACK_FLOOR_MS);
  }
  ASSERT_EQ(HubFault::RELAY_CONTESTED, rt->fault());

  entities().room.publish_state(35.f);
  tick(t += 1000);
  EXPECT_EQ(HubFault::OVERTEMP, rt->fault());
  EXPECT_EQ(HubAction::OFF, rt->action());
  EXPECT_FALSE(entities().relay1.state);
  for (int pass = 0; pass < 3; pass++) {
    entities().relay1.turn_on();
    tick(t += 1000);
    EXPECT_FALSE(entities().relay1.state) << "pass " << pass;
  }

  LogCapture::instance().clear();
  entities().room.publish_state(18.f);
  tick(t += 1000);
  EXPECT_EQ(HubFault::RELAY_CONTESTED, rt->fault()) << "the contest is still on";
  EXPECT_TRUE(LogCapture::instance().has("'loop': overtemp cleared"));
  EXPECT_TRUE(LogCapture::instance().has("'loop': relay_contested"));
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_TRUE(entities().relay1.state);
}

// A silent sensor is a cut-out too: a close from elsewhere is opened on every pass.
TEST_F(ControlLoop, AStaleSensorOpensEveryCloseAtOnce) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.safety.sensor_timeout_s = 10.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(hub().ms + 11000);
  ASSERT_EQ(HubFault::SENSOR_STALE, rt->fault());
  for (int pass = 0; pass < 3; pass++) {
    entities().relay1.turn_on();
    tick(hub().ms + 1000);
    EXPECT_FALSE(entities().relay1.state) << "pass " << pass;
  }
}

// Before its first reading the thermostat has nothing to heat for: a close from elsewhere is
// opened on every pass, and each one is counted.
TEST_F(ControlLoop, NoReadingYetOpensEveryCloseAtOnce) {
  this->id_ = this->create(this->base(ControlKind::BANG_BANG)).id;
  ControllerRuntime *rt = hub().runtime_of(this->id_);
  tick(100000);
  ASSERT_FALSE(rt->has_sample());
  for (uint32_t n = 1; n <= CONTEST_MOVES + 2; n++) {
    entities().relay1.turn_on();
    tick(100000 + n * 1000);
    EXPECT_FALSE(entities().relay1.state) << "pass " << n;
    EXPECT_EQ(n, hub().claim("relay_1")->moves());
  }
}

// Mode off holds the relay open: a close from elsewhere is put back, paced as in any mode.
TEST_F(ControlLoop, ModeOffKeepsTheRelayOpenAndPutsItBack) {
  ControllerRuntime *rt = this->start(this->base(ControlKind::BANG_BANG), 18.f);
  tick(200000);
  call(hub().entity_of(this->id_), climate::CLIMATE_MODE_OFF);
  tick(201000);
  ASSERT_FALSE(entities().relay1.state);

  entities().relay1.turn_on();
  tick(202000);
  EXPECT_FALSE(entities().relay1.state) << "the first close goes back at once";
  entities().relay1.turn_on();
  tick(203000);
  EXPECT_TRUE(entities().relay1.state) << "the second waits the floor";
  tick(203000 + PUT_BACK_FLOOR_MS - 1);
  EXPECT_TRUE(entities().relay1.state);
  tick(203000 + PUT_BACK_FLOOR_MS);
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_EQ("loop", hub().claimed_by("relay_1"));
  EXPECT_EQ(HubFault::NONE, rt->fault());
  EXPECT_EQ(HubAction::OFF, rt->action());
}

// Only a stop frees the relay: from then on nothing puts it back.
TEST_F(ControlLoop, AStopFreesTheRelay) {
  this->start(this->base(ControlKind::BANG_BANG), 18.f);
  tick(200000);
  ASSERT_TRUE(hub().set_enabled(this->id_, false).ok);
  EXPECT_EQ("", hub().claimed_by("relay_1"));

  entities().relay1.turn_on();
  tick(201000);
  tick(220000);
  EXPECT_TRUE(entities().relay1.state);
}

// A thermostat that takes the relay over starts with no moves held against it, and a move it
// finds at its first look, made after the holder's last pass, is none either.
TEST_F(ControlLoop, ATakeOverStartsWithNoMovesAgainstTheTaker) {
  ClimateConfig winter = this->base(ControlKind::BANG_BANG);
  winter.name = "Winter";
  this->start(winter, 18.f);
  tick(200000);
  uint32_t t = 200000;
  for (uint32_t n = 1; n < CONTEST_MOVES; n++) {
    entities().relay1.turn_off();
    tick(t += 1000);
    tick(t += PUT_BACK_FLOOR_MS);
  }
  ASSERT_EQ(CONTEST_MOVES - 1, hub().claim("relay_1")->moves());
  ClimateConfig summer = winter;
  summer.name = "Summer";
  summer.enabled = false;
  this->create(summer);

  entities().relay1.turn_off();
  ASSERT_TRUE(hub().set_enabled("summer", true, true).ok);
  EXPECT_EQ(0u, hub().claim("relay_1")->moves());
  tick(t += 1000);
  EXPECT_TRUE(entities().relay1.state) << "found moved, it goes back";
  EXPECT_EQ(0u, hub().claim("relay_1")->moves()) << "and is no move";
  entities().relay1.turn_off();
  tick(t += 1000);
  EXPECT_TRUE(entities().relay1.state) << "the taker's first move goes back at once";
  EXPECT_EQ(1u, hub().claim("relay_1")->moves());
  EXPECT_EQ(HubFault::NONE, hub().runtime_of("summer")->fault());
}

// A Save keeps the claim, and the moves held against it with the claim.
TEST_F(ControlLoop, ASaveKeepsTheMoves) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(200000);
  uint32_t t = 200000;
  for (uint32_t n = 1; n <= CONTEST_MOVES; n++) {
    entities().relay1.turn_off();
    tick(t += 1000);
    tick(t += PUT_BACK_FLOOR_MS);
  }
  ASSERT_EQ(HubFault::RELAY_CONTESTED, rt->fault());
  config.update_interval_s = 2.f;
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  tick(t += 1000);
  EXPECT_EQ(HubFault::RELAY_CONTESTED, rt->fault());
}

// A Save that drops a relay whose put-back waits opens it at once: let go, nothing would open it
// later, and it would stay closed with no thermostat on it.
TEST_F(ControlLoop, ASaveThatDropsARelayWhosePutBackWaitsOpensItAtOnce) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  this->start(config, 22.f);
  tick(200000);
  ASSERT_FALSE(entities().relay1.state);
  entities().relay1.turn_on();
  tick(201000);
  ASSERT_FALSE(entities().relay1.state) << "the first close goes back at once";
  entities().relay1.turn_on();
  tick(202000);
  ASSERT_TRUE(entities().relay1.state) << "the second waits the floor";

  config.heat.relay_id = "relay_2";
  ASSERT_TRUE(hub().update(this->id_, config).ok);
  EXPECT_EQ("", hub().claimed_by("relay_1"));
  EXPECT_FALSE(entities().relay1.state);
}

// --- The action follows the relays ---

// A thermostat that wants heat while min_off holds its relay open is idle, and says what holds
// the relay; it turns heating as the relay closes, and Home Assistant never sees it earlier.
TEST_F(ControlLoop, MinOffKeepsItIdleUntilTheRelayCloses) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.heat.min_off_s = 60.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  HubClimate *entity = hub().entity_of(this->id_);
  tick(200000);
  entities().room.publish_state(22.f);
  tick(201000);
  ASSERT_FALSE(entities().relay1.state);
  ASSERT_EQ(HubAction::IDLE, rt->action());

  Actions &seen = watch(entity);
  entities().room.publish_state(18.f);
  tick(202000);
  EXPECT_FALSE(entities().relay1.state) << "opened at 201 s, it stays open until 261 s";
  EXPECT_EQ(HubAction::IDLE, rt->action());
  EXPECT_EQ(RelayWait::MIN_OFF, rt->heat_relay_wait());
  tick(260999);
  EXPECT_EQ(HubAction::IDLE, rt->action());
  tick(261000);
  EXPECT_TRUE(entities().relay1.state);
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_EQ(RelayWait::NONE, rt->heat_relay_wait());
  EXPECT_EQ((Actions{climate::CLIMATE_ACTION_IDLE, climate::CLIMATE_ACTION_HEATING}), seen)
      << "the reading, then the close";
}

// The other way round: min_on holds a relay the thermostat let go closed, and it heats on.
TEST_F(ControlLoop, MinOnKeepsItHeatingUntilTheRelayOpens) {
  ClimateConfig config = this->base(ControlKind::BANG_BANG);
  config.heat.min_on_s = 60.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  HubClimate *entity = hub().entity_of(this->id_);
  tick(200000);
  ASSERT_EQ(HubAction::HEATING, rt->action());

  Actions &seen = watch(entity);
  entities().room.publish_state(22.f);
  tick(201000);
  EXPECT_TRUE(entities().relay1.state) << "closed at 200 s, it stays closed until 260 s";
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_EQ(RelayWait::MIN_ON, rt->heat_relay_wait());
  tick(259999);
  EXPECT_EQ(HubAction::HEATING, rt->action());
  tick(260000);
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_EQ(HubAction::IDLE, rt->action());
  EXPECT_EQ(RelayWait::NONE, rt->heat_relay_wait());
  EXPECT_EQ((Actions{climate::CLIMATE_ACTION_HEATING, climate::CLIMATE_ACTION_IDLE}), seen);
}

// A PID too is idle until a pulse closes the relay. From then on it heats between its pulses,
// also while min_off stretches a gap past the period's edge, until its duty drops to zero.
TEST_F(ControlLoop, APidIsIdleUntilAPulseClosesTheRelay) {
  ClimateConfig config = this->base(ControlKind::PID);
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.f;
  config.heat.min_off_s = 60.f;
  ControllerRuntime *rt = this->start(config, 20.f);
  tick(200000);
  ASSERT_TRUE(entities().relay1.state) << "open since the boot, more than 60 s ago";
  entities().room.publish_state(26.f);
  tick(201000);
  ASSERT_FALSE(entities().relay1.state);
  ASSERT_EQ(HubAction::IDLE, rt->action());

  // Half power on a ten second period that started at 200 s: the PWM wants the relay closed.
  entities().room.publish_state(20.f);
  tick(202000);
  EXPECT_NEAR(0.5f, rt->heat_duty(), 1e-4f);
  EXPECT_FALSE(entities().relay1.state) << "opened at 201 s, it stays open until 261 s";
  EXPECT_EQ(HubAction::IDLE, rt->action());
  EXPECT_EQ(RelayWait::MIN_OFF, rt->heat_relay_wait());
  tick(261000);
  EXPECT_TRUE(entities().relay1.state);
  EXPECT_EQ(HubAction::HEATING, rt->action());

  tick(266000);
  EXPECT_FALSE(entities().relay1.state) << "past the half period";
  EXPECT_EQ(HubAction::HEATING, rt->action()) << "between two pulses";
  EXPECT_EQ(RelayWait::NONE, rt->heat_relay_wait()) << "the PWM opened it";
  tick(270000);
  EXPECT_FALSE(entities().relay1.state) << "the next pulse waits for 326 s";
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_EQ(RelayWait::MIN_OFF, rt->heat_relay_wait());

  entities().room.publish_state(26.f);
  tick(271000);
  EXPECT_EQ(HubAction::IDLE, rt->action());
  EXPECT_EQ(RelayWait::NONE, rt->heat_relay_wait());
}

// A PID's gap between pulses is heating only while the mode heats: a mode from Home Assistant
// that no longer does ends it at once, not at the next pass.
TEST_F(ControlLoop, AModeThatStopsHeatingEndsAPidsGapAtOnce) {
  ClimateConfig config = with_cooling(this->base(ControlKind::PID), true);
  config.setpoint = 25.f;
  config.pid.kp = 0.1f;
  config.pid.ki = 0.f;
  ControllerRuntime *rt = this->start(config, 20.f);
  HubClimate *entity = hub().entity_of(this->id_);
  tick(200000);
  tick(206000);
  ASSERT_FALSE(entities().relay1.state);
  ASSERT_EQ(HubAction::HEATING, rt->action());

  Actions &seen = watch(entity);
  call(entity, climate::CLIMATE_MODE_COOL);
  EXPECT_EQ(Actions{climate::CLIMATE_ACTION_IDLE}, seen);
}

// Switching over from one relay to the other, min_on may hold the first closed while the second
// closes: the action is the relay the thermostat drives now, either way round.
TEST_F(ControlLoop, ASwitchOverShowsTheRelayItDrivesNow) {
  ClimateConfig config = with_cooling(this->base(ControlKind::BANG_BANG), true);
  config.heat.min_on_s = 60.f;
  config.cool.min_on_s = 60.f;
  ControllerRuntime *rt = this->start(config, 18.f);
  tick(200000);
  ASSERT_TRUE(entities().relay1.state);

  entities().room.publish_state(25.f);
  tick(201000);
  EXPECT_TRUE(entities().relay1.state) << "min_on holds the heater until 260 s";
  EXPECT_TRUE(entities().relay2.state);
  EXPECT_EQ(HubAction::COOLING, rt->action());
  EXPECT_EQ(RelayWait::MIN_ON, rt->heat_relay_wait());
  EXPECT_EQ(RelayWait::NONE, rt->cool_relay_wait());

  entities().room.publish_state(18.f);
  tick(230000);
  EXPECT_TRUE(entities().relay1.state);
  EXPECT_TRUE(entities().relay2.state) << "min_on holds the cooler until 261 s";
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_EQ(RelayWait::NONE, rt->heat_relay_wait());
  EXPECT_EQ(RelayWait::MIN_ON, rt->cool_relay_wait());

  tick(261000);
  EXPECT_FALSE(entities().relay2.state);
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_EQ(RelayWait::NONE, rt->cool_relay_wait());
}

// A relay closed from elsewhere heats, whoever closed it, until it is put back; the wait for the
// put-back is the move's, not the thermostat's own.
TEST_F(ControlLoop, ARelayClosedFromElsewhereShowsUntilItIsPutBack) {
  ControllerRuntime *rt = this->start(this->base(ControlKind::BANG_BANG), 22.f);
  tick(200000);
  ASSERT_FALSE(entities().relay1.state);
  entities().relay1.turn_on();
  tick(201000);
  ASSERT_FALSE(entities().relay1.state) << "the first close goes back at once";
  EXPECT_EQ(HubAction::IDLE, rt->action());

  entities().relay1.turn_on();
  tick(202000);
  ASSERT_TRUE(entities().relay1.state) << "the second waits the floor";
  EXPECT_EQ(HubAction::HEATING, rt->action());
  EXPECT_EQ(RelayWait::NONE, rt->heat_relay_wait());
  tick(202000 + PUT_BACK_FLOOR_MS);
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_EQ(HubAction::IDLE, rt->action());
}

// Mode off and a cut-out open the relay without waiting for min_on, so nothing waits on it.
TEST_F(ControlLoop, NoWaitWhileTheRelaysAreHeldOpen) {
  for (bool cut_out : {false, true}) {
    SCOPED_TRACE(cut_out ? "overtemp" : "mode off");
    ClimateConfig config = this->base(ControlKind::BANG_BANG);
    config.heat.min_on_s = 60.f;
    config.safety.max_temperature = 30.f;
    ControllerRuntime *rt = this->start(config, 18.f);
    tick(200000);
    entities().room.publish_state(22.f);
    tick(201000);
    ASSERT_EQ(RelayWait::MIN_ON, rt->heat_relay_wait());

    if (cut_out) {
      entities().room.publish_state(35.f);
    } else {
      call(hub().entity_of(this->id_), climate::CLIMATE_MODE_OFF);
    }
    tick(202000);
    EXPECT_FALSE(entities().relay1.state);
    EXPECT_EQ(HubAction::OFF, rt->action());
    EXPECT_EQ(RelayWait::NONE, rt->heat_relay_wait());
    EXPECT_EQ(RelayWait::NONE, rt->cool_relay_wait()) << "no cooling relay";
    ASSERT_TRUE(hub().remove(this->id_).ok);
    reset_entities();
    hub().ms = 100000;
  }
}

// The runtime on its own: with no reading every close from elsewhere is undone at once; in mode
// off with a reading it is put back paced.
TEST(ControllerRuntimeAlone, OnlyModeOffWithAReadingPacesThePutBack) {
  HubClimate entity(&hub(), 200);
  FakeSwitch heat;
  RelayClaim claim(&heat, "alone");
  claim.resume({false, 0});
  ControllerRuntime rt(&entity);
  ClimateConfig config = draft("Alone");
  sensor::Sensor probe;
  rt.start(&config, &probe, &claim, nullptr, 1000);
  rt.tick(1000);
  for (uint32_t t = 2000; t <= 5000; t += 1000) {
    heat.turn_on();
    rt.tick(t);
    EXPECT_EQ(HubFault::NONE, rt.fault());
    EXPECT_FALSE(heat.state) << "at " << t;
  }
  EXPECT_EQ(4u, claim.moves());

  RelayClaim fresh(&heat, "alone");
  fresh.resume({false, 0});
  config.mode = HubMode::OFF;
  // A reading handed over rather than sampled: a sample would publish an entity App never set up.
  rt.start(&config, &probe, &fresh, nullptr, 5000, Reading{18.f, 5000, true});
  rt.tick(5500);
  heat.turn_on();
  rt.tick(6000);
  EXPECT_FALSE(heat.state) << "the first close goes back at once";
  heat.turn_on();
  rt.tick(7000);
  EXPECT_TRUE(heat.state) << "the second waits the floor";
  rt.stop(8000);
  EXPECT_FALSE(heat.state) << "a stop does not wait";
}

}  // namespace esphome::climate_hub::testing
