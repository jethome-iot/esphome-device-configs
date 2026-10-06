// A calibration end to end: a room with dead time answers the relay, the hub swings it, and the
// gains land in the thermostat's file; and every way a run can end before that.
#include <functional>
#include "common.h"
#include "esphome/components/climate_hub/param_table.h"
#include "room.h"

namespace esphome::climate_hub::testing {
namespace {

class Calibration : public HubTest {
 protected:
  // A PID thermostat in heat on Room and Relay 1, target 21.
  static ClimateConfig living_room() {
    ClimateConfig c = draft("Living room");
    c.setpoint = 21.f;
    return c;
  }

  // Creates `config` and gives it a first reading: a thermostat that runs and controls.
  std::string start(const ClimateConfig &config, float reading = 18.f) {
    const std::string id = this->create(config).id;
    hub().ms += 1000;
    entities().room.publish_state(reading);
    hub().loop();
    return id;
  }

  Result calibrate(const std::string &id, optional<AutotuneDirection> direction = nullopt,
                   AutotuneRule rule = AutotuneRule::ZN_PI) {
    return hub().start_autotune(id, direction, rule);
  }

  // Steps `room` on Relay 1 (or Relay 2 when `cooling`) in its own step, a reading after each
  // loop pass, for up to `hours` or until `done` says so.
  static void swing(Room &room, const RoomModel &model, float hours, const std::function<bool()> &done = nullptr,
                    bool cooling = false) {
    const auto step_ms = static_cast<uint32_t>(model.step_s * 1000.f);
    const auto steps = static_cast<uint32_t>(hours * 3600.f / model.step_s);
    for (uint32_t i = 0; i < steps; i++) {
      hub().ms += step_ms;
      hub().loop();
      const bool on = cooling ? entities().relay2.state : entities().relay1.state;
      entities().room.publish_state(room.step(on));
      if (done && done())
        return;
    }
  }

  // Readings by hand, `every_s` apart, `count` of them.
  static void hold(float reading, uint32_t every_s, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
      hub().ms += every_s * 1000;
      hub().loop();
      entities().room.publish_state(reading);
    }
    hub().loop();
  }

  // Every action `entity` publishes from here until the next call. A slot outlives the test and
  // its callbacks cannot be removed, so each entity is hooked once.
  static std::vector<climate::ClimateAction> &published(HubClimate *entity) {
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

  static bool ended(const std::string &id) {
    const AutotuneRun *run = hub().autotune(id);
    return run != nullptr && !run->running();
  }

  // A file's gains and revision, as the next boot reads them.
  ClimateConfig on_flash(const std::string &id) {
    ClimateConfig config;
    std::string error;
    EXPECT_TRUE(from_json(read_file(this->file_of(id)), &config, &error)) << error;
    return config;
  }
};

}  // namespace

// The owner's figure for radiators: about an hour and a half.
TEST_F(Calibration, ARoomWithRadiatorsGetsItsGainsInAboutTwoHours) {
  const RoomModel model = radiator_room();
  Room room(model);
  const std::string id = this->start(living_room(), room.reading());
  ASSERT_TRUE(this->calibrate(id).ok);
  const AutotuneRun *run = hub().autotune(id);
  ASSERT_NE(nullptr, run);
  EXPECT_EQ(AutotuneDirection::HEAT, run->direction()) << "mode heat calibrates heating";
  const uint32_t started = hub().ms;

  swing(room, model, 24.f, [&id] { return ended(id); });
  ASSERT_EQ(AutotuneState::SUCCEEDED, run->state());
  EXPECT_EQ(AutotuneEnd::NONE, run->reason());
  const float hours = (hub().ms - started) / 3600000.f;
  EXPECT_GT(hours, 1.f);
  EXPECT_LT(hours, 3.f);
  EXPECT_EQ(run->elapsed_ms(hub().ms + 60000), run->elapsed_ms(hub().ms)) << "an ended run's clock stops";
  EXPECT_EQ(6u, run->tuner().extremes().size());
  EXPECT_FALSE(run->tuner().noisy());

  // ZN PI from the measured Ku and Pu, to the six decimals the file keeps, written and run.
  const float ku = run->tuner().ku();
  const float pu = run->tuner().pu();
  EXPECT_NEAR(0.45f * ku, run->new_gains().kp, 5e-7f);
  EXPECT_NEAR(0.54f * ku / pu, run->new_gains().ki, 5e-7f);
  EXPECT_EQ(0.f, run->new_gains().kd);
  EXPECT_EQ(0.6f, run->old_gains().kp) << "what it replaced: the defaults";
  EXPECT_EQ(0.0025f, run->old_gains().ki);
  EXPECT_TRUE(run->persisted());
  EXPECT_FALSE(run->asymmetric());
  EXPECT_FALSE(run->uneven());
  EXPECT_FALSE(run->clamped());

  const ClimateConfig *stored = hub().store().get(id);
  EXPECT_EQ(run->new_gains().kp, stored->pid.kp);
  EXPECT_EQ(1u, stored->revision);
  const ClimateConfig flash = this->on_flash(id);
  EXPECT_EQ(run->new_gains().kp, flash.pid.kp);
  EXPECT_EQ(run->new_gains().ki, flash.pid.ki);
  EXPECT_EQ(0.f, flash.pid.kd);
  EXPECT_EQ(1u, flash.revision);

  // Back to the PID with them, from a clean start: its first pass integrates nothing.
  ControllerRuntime *rt = hub().runtime_of(id);
  EXPECT_EQ(nullptr, rt->autotune());
  hub().ms += 1000;
  hub().loop();
  EXPECT_FLOAT_EQ(stored->pid.kp * rt->pid().error(), rt->pid().proportional_term());
  EXPECT_EQ(0.f, rt->pid().integral_term());
}

// The owner's figure for a floor: eight to ten hours. A slow floor's gains need the wide ki and
// kd ranges.
TEST_F(Calibration, AFloorHeatingTakesEightToTenHours) {
  const RoomModel model = floor_heating();
  Room room(model);
  ClimateConfig config = living_room();
  config.heat.min_on_s = 60.f;
  config.heat.min_off_s = 60.f;
  const std::string id = this->start(config, room.reading());
  ASSERT_TRUE(this->calibrate(id, AutotuneDirection::HEAT, AutotuneRule::SOME_OVERSHOOT).ok);
  const uint32_t started = hub().ms;
  swing(room, model, 24.f, [&id] { return ended(id); });

  const AutotuneRun *run = hub().autotune(id);
  ASSERT_EQ(AutotuneState::SUCCEEDED, run->state());
  const float hours = (hub().ms - started) / 3600000.f;
  EXPECT_GT(hours, 7.f);
  EXPECT_LT(hours, 11.f);
  EXPECT_LT(run->new_gains().ki, 0.0001f) << "under the old step";
  EXPECT_FALSE(run->clamped());
  EXPECT_NEAR(0.111f * run->tuner().ku() * run->tuner().pu(), hub().store().get(id)->pid.kd, 1e-3f);
}

// Full or nothing at the band's edges, and Home Assistant sees heating or idle as from any PID.
TEST_F(Calibration, TheRelayGoesFullOrOffAtTheBand) {
  const RoomModel model = radiator_room();
  Room room(model);
  const std::string id = this->start(living_room(), room.reading());
  ASSERT_TRUE(this->calibrate(id).ok);
  ControllerRuntime *rt = hub().runtime_of(id);
  HubClimate *entity = hub().entity_of(id);
  int switched_on_below = 0;
  int switched_off_above = 0;
  bool was = entities().relay1.state;
  swing(room, model, 24.f, [&] {
    if (ended(id))
      return true;
    EXPECT_TRUE(rt->heat_duty() == 0.f || rt->heat_duty() == 1.f) << rt->heat_duty();
    EXPECT_EQ(rt->heat_duty() > 0.f ? climate::CLIMATE_ACTION_HEATING : climate::CLIMATE_ACTION_IDLE, entity->action);
    const bool now = entities().relay1.state;
    if (now != was) {
      const float t = entity->current_temperature;
      if (now)
        switched_on_below += t < 21.f - AUTOTUNE_NOISEBAND ? 1 : 0;
      else
        switched_off_above += t > 21.f + AUTOTUNE_NOISEBAND ? 1 : 0;
      was = now;
    }
    return false;
  });
  // The relay follows the reading that crossed, a pass later. The PID had it closed at 18 °C
  // already, and the sixth switch is the PID's to make.
  EXPECT_EQ(2, switched_on_below);
  EXPECT_EQ(3, switched_off_above);
}

// Every reading moves the relay, not every update_interval_s.
TEST_F(Calibration, EveryReadingIsFedNotEveryInterval) {
  ClimateConfig config = living_room();
  config.update_interval_s = 3600.f;
  const std::string id = this->start(config, 21.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  hold(21.f, 10, 1);
  EXPECT_FALSE(entities().relay1.state) << "at the target it starts off";
  hold(20.7f, 10, 1);
  EXPECT_TRUE(entities().relay1.state) << "ten seconds later, under the band";
  hold(21.3f, 10, 1);
  EXPECT_FALSE(entities().relay1.state);
}

// The relay's dwell holds, as at any other time.
TEST_F(Calibration, MinOnAndMinOffHold) {
  ClimateConfig config = living_room();
  config.heat.min_on_s = 120.f;
  config.heat.min_off_s = 300.f;
  const std::string id = this->start(config, 21.f);
  hold(21.f, 10, 40);
  ASSERT_TRUE(this->calibrate(id).ok);
  hold(20.f, 10, 1);
  ASSERT_TRUE(entities().relay1.state);
  hold(22.f, 10, 1);
  EXPECT_TRUE(entities().relay1.state) << "closed 10 s ago, min_on holds it";
  hold(22.f, 10, 11);
  EXPECT_FALSE(entities().relay1.state) << "and lets it go after 120 s";
  hold(20.f, 10, 1);
  EXPECT_FALSE(entities().relay1.state) << "min_off holds it open";
  hold(20.f, 10, 29);
  EXPECT_TRUE(entities().relay1.state);
}

// heat_cool asks which; the other relay stays open, as the cool run's relay is the cooling one.
TEST_F(Calibration, InHeatCoolItSwingsTheRelayItIsToldTo) {
  ClimateConfig config = living_room();
  config.cool.relay_id = "relay_2";
  config.mode = HubMode::HEAT_COOL;
  const std::string id = this->start(config, 21.f);
  Result refused = this->calibrate(id);
  EXPECT_EQ(400, refused.code);
  EXPECT_EQ("In heat_cool, direction must say heat or cool", refused.error);

  ASSERT_TRUE(this->calibrate(id, AutotuneDirection::COOL).ok);
  hold(21.5f, 10, 1);
  EXPECT_TRUE(entities().relay2.state) << "too warm: full cooling";
  EXPECT_FALSE(entities().relay1.state) << "the heating relay stays open";
  EXPECT_EQ(climate::CLIMATE_ACTION_COOLING, hub().entity_of(id)->action);
  hold(20.5f, 10, 1);
  EXPECT_FALSE(entities().relay2.state);
  EXPECT_FALSE(entities().relay1.state) << "even under the band";
}

TEST_F(Calibration, ACoolingRoomIsCalibratedWithItsCoolingRelay) {
  RoomModel model = radiator_room();
  model.start = 27.f;
  model.outside = 32.f;
  model.drive = -10.f;
  Room room(model);
  ClimateConfig config = living_room();
  config.heat.relay_id = "";
  config.cool.relay_id = "relay_2";
  config.mode = HubMode::COOL;
  config.setpoint = 24.f;
  const std::string id = this->start(config, room.reading());
  ASSERT_TRUE(this->calibrate(id).ok);
  EXPECT_EQ(AutotuneDirection::COOL, hub().autotune(id)->direction()) << "mode cool calibrates cooling";
  const auto done = [&id] { return ended(id); };
  swing(room, model, 24.f, done, true);
  EXPECT_EQ(AutotuneState::SUCCEEDED, hub().autotune(id)->state());
  EXPECT_GT(hub().store().get(id)->pid.kp, 0.f) << "the gains are the cooler's, positive as for heating";
}

// --- What ends a run ---

TEST_F(Calibration, CancelEndsItAndThePidTakesOver) {
  const std::string id = this->start(living_room(), 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  hold(18.f, 10, 3);
  ASSERT_TRUE(entities().relay1.state);
  ASSERT_TRUE(hub().cancel_autotune(id).ok);
  const AutotuneRun *run = hub().autotune(id);
  EXPECT_EQ(AutotuneState::FAILED, run->state());
  EXPECT_EQ(AutotuneEnd::CANCELLED, run->reason());
  EXPECT_EQ(30000u, run->elapsed_ms(hub().ms + 5000));
  EXPECT_EQ(0.6f, hub().store().get(id)->pid.kp) << "the gains stay";
  EXPECT_EQ(0u, hub().store().get(id)->revision);

  Result again = hub().cancel_autotune(id);
  EXPECT_EQ(409, again.code);
  EXPECT_EQ("No calibration is running", again.error);
  EXPECT_EQ(404, hub().cancel_autotune("attic").code);
}

TEST_F(Calibration, ANewTargetEndsItAndTheSameOneDoesNot) {
  const std::string id = this->start(living_room(), 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  ASSERT_TRUE(hub().set_setpoint(id, 21.f).ok);
  EXPECT_TRUE(hub().autotune(id)->running()) << "the target it has";
  ASSERT_TRUE(hub().set_setpoint(id, 22.f).ok);
  EXPECT_EQ(AutotuneEnd::TARGET_CHANGED, hub().autotune(id)->reason());
}

TEST_F(Calibration, AModeFromHomeAssistantEndsIt) {
  ClimateConfig config = living_room();
  config.cool.relay_id = "relay_2";
  const std::string id = this->start(config, 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  auto call = hub().entity_of(id)->make_call();
  call.set_mode(climate::CLIMATE_MODE_HEAT_COOL);
  call.perform();
  EXPECT_EQ(AutotuneEnd::MODE_CHANGED, hub().autotune(id)->reason());
  EXPECT_FALSE(entities().relay2.state);
}

TEST_F(Calibration, APresetThatMovesTheTargetEndsIt) {
  ClimateConfig config = living_room();
  PresetConfig same;
  same.name = "Same";
  same.setpoint = 21.f;
  PresetConfig eco;
  eco.name = "Eco";
  eco.setpoint = 18.f;
  config.presets = {same, eco};
  const std::string id = this->start(config, 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  ASSERT_TRUE(hub().apply_preset(id, "same").ok);
  EXPECT_TRUE(hub().autotune(id)->running()) << "a label alone moves nothing";
  ASSERT_TRUE(hub().apply_preset(id, "eco").ok);
  EXPECT_EQ(AutotuneEnd::TARGET_CHANGED, hub().autotune(id)->reason());
}

// The runtime ends it itself, so a caller that reaches it past the hub's entity calls ends it too.
TEST_F(Calibration, ATargetOrAModeSetOnTheRuntimeItselfEndsIt) {
  ClimateConfig config = living_room();
  config.cool.relay_id = "relay_2";
  PresetConfig eco;
  eco.name = "Eco";
  eco.setpoint = 18.f;
  config.presets = {eco};
  const std::string id = this->start(config, 18.f);
  ControllerRuntime *rt = hub().runtime_of(id);
  HubClimate *entity = hub().entity_of(id);

  ASSERT_TRUE(this->calibrate(id).ok);
  hold(18.f, 10, 1);
  ASSERT_TRUE(entities().relay1.state);
  auto same = entity->make_call();
  same.set_target_temperature(21.f);
  rt->control(same, hub().ms);
  EXPECT_TRUE(hub().autotune(id)->running()) << "the target it has";
  auto target = entity->make_call();
  target.set_target_temperature(22.f);
  rt->control(target, hub().ms + 500);
  EXPECT_EQ(AutotuneEnd::TARGET_CHANGED, hub().autotune(id)->reason());
  EXPECT_EQ(10500u, hub().autotune(id)->elapsed_ms(hub().ms + 60000)) << "ended at the clock it was given";
  EXPECT_EQ(nullptr, rt->autotune());
  EXPECT_EQ(climate::CLIMATE_ACTION_IDLE, entity->action) << "the run's relay is not the PID's";

  ASSERT_TRUE(this->calibrate(id).ok);
  auto mode = entity->make_call();
  mode.set_mode(climate::CLIMATE_MODE_HEAT_COOL);
  rt->control(mode, hub().ms);
  EXPECT_EQ(AutotuneEnd::MODE_CHANGED, hub().autotune(id)->reason());

  ASSERT_TRUE(this->calibrate(id, AutotuneDirection::HEAT).ok);
  rt->pick_preset(*hub().store().get(id)->find_preset("eco"), hub().ms);
  EXPECT_EQ(AutotuneEnd::TARGET_CHANGED, hub().autotune(id)->reason());
}

TEST_F(Calibration, ASaveEndsItAndARefusedOneDoesNot) {
  const std::string id = this->start(living_room(), 18.f);
  this->create(draft("Hall heating", "relay_2"));
  ASSERT_TRUE(this->calibrate(id).ok);
  ClimateConfig doc = *hub().store().get(id);
  doc.name = "Hall";
  EXPECT_EQ(409, hub().update(id, doc).code) << "a YAML climate's name";
  EXPECT_TRUE(hub().autotune(id)->running());
  doc.name = "Living room";
  ASSERT_TRUE(hub().update(id, doc).ok);
  EXPECT_EQ(AutotuneEnd::SAVED, hub().autotune(id)->reason());
  EXPECT_TRUE(hub().is_running(id));
}

TEST_F(Calibration, DisablingStopsItAndTheResultStays) {
  const std::string id = this->start(living_room(), 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  ASSERT_TRUE(hub().set_enabled(id, false).ok);
  ASSERT_NE(nullptr, hub().autotune(id)) << "kept until the next run, a delete or a reboot";
  EXPECT_EQ(AutotuneEnd::STOPPED, hub().autotune(id)->reason());
  EXPECT_FALSE(entities().relay1.state);
}

TEST_F(Calibration, DeletingForgetsIt) {
  const std::string id = this->start(living_room(), 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  ASSERT_TRUE(hub().remove(id).ok);
  EXPECT_EQ(nullptr, hub().autotune(id));
}

TEST_F(Calibration, ATakeOverEndsIt) {
  const std::string id = this->start(living_room(), 18.f);
  ClimateConfig winter = draft("Winter");
  winter.enabled = false;
  const std::string other = this->create(winter).id;
  ASSERT_TRUE(this->calibrate(id).ok);
  ASSERT_TRUE(hub().set_enabled(other, true, true).ok);
  EXPECT_EQ(AutotuneEnd::TAKEN_OVER, hub().autotune(id)->reason());
}

TEST_F(Calibration, ASilentSensorEndsIt) {
  const std::string id = this->start(living_room(), 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  hub().ms += 301000;
  hub().loop();
  EXPECT_EQ(AutotuneEnd::SENSOR_STALE, hub().autotune(id)->reason());
}

TEST_F(Calibration, TheCutOutStaysLiveAndEndsIt) {
  ClimateConfig config = living_room();
  config.safety.max_temperature = 25.f;
  const std::string id = this->start(config, 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  hold(18.f, 10, 1);
  ASSERT_TRUE(entities().relay1.state);
  hold(26.f, 10, 1);
  EXPECT_EQ(AutotuneEnd::OVERTEMP, hub().autotune(id)->reason());
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_EQ(HubFault::OVERTEMP, hub().runtime_of(id)->fault());
}

TEST_F(Calibration, ARelaySwitchedFromElsewhereEndsIt) {
  const std::string id = this->start(living_room(), 21.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  // Closed every 20 s: every other one is a move, the others find it closed still.
  for (int i = 0; i < 20 && !ended(id); i++) {
    entities().relay1.turn_on();
    hold(21.f, 20, 1);
  }
  EXPECT_EQ(AutotuneEnd::RELAY_CONTESTED, hub().autotune(id)->reason());
}

// The relay switches every five hours: no stall, but no result in a day either.
TEST_F(Calibration, ADayEndsIt) {
  const std::string id = this->start(living_room(), 21.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  for (int phase = 0; phase < 6 && !ended(id); phase++)
    hold(phase % 2 == 0 ? 20.5f : 21.5f, 60, 300);
  const AutotuneRun *run = hub().autotune(id);
  EXPECT_EQ(AutotuneEnd::TIMEOUT, run->reason());
  EXPECT_EQ(5u, run->tuner().phase_count());
  EXPECT_LE(run->elapsed_ms(hub().ms), AUTOTUNE_MAX_MS + 61000);
}

// A probe that hovers at the target crosses it reading after reading and never swings the relay.
TEST_F(Calibration, ANoisyProbeAtTheTargetEndsIt) {
  const std::string id = this->start(living_room(), 21.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  // The first crossing starts the clock; each after it is one interval.
  for (uint32_t i = 0; i <= PidAutotuner::MAX_INTERVALS; i++)
    hold(i % 2 == 0 ? 20.9f : 21.1f, 10, 1);
  EXPECT_TRUE(hub().autotune(id)->running());
  hold(21.1f, 10, 1);
  EXPECT_EQ(AutotuneEnd::NOISY, hub().autotune(id)->reason());
  EXPECT_EQ(nullptr, hub().runtime_of(id)->autotune()) << "back to the PID";
  EXPECT_EQ(0u, hub().autotune(id)->tuner().phase_count());
}

// A heater that cannot lift the room across the band.
TEST_F(Calibration, SixHoursWithoutASwitchEndIt) {
  const std::string id = this->start(living_room(), 19.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  hold(19.f, 60, 359);
  EXPECT_TRUE(hub().autotune(id)->running());
  hold(19.f, 60, 2);
  EXPECT_EQ(AutotuneEnd::NO_SWITCH, hub().autotune(id)->reason());
}

TEST_F(Calibration, ARebootForgetsItButNotTheGains) {
  const RoomModel model = radiator_room();
  Room room(model);
  const std::string id = this->start(living_room(), room.reading());
  ASSERT_TRUE(this->calibrate(id).ok);
  swing(room, model, 24.f, [&id] { return ended(id); });
  const float kp = hub().store().get(id)->pid.kp;
  this->reboot();
  EXPECT_EQ(nullptr, hub().autotune(id));
  EXPECT_EQ(kp, hub().store().get(id)->pid.kp);
  EXPECT_EQ(1u, hub().store().get(id)->revision);
}

TEST_F(Calibration, AResultTheFileDidNotTakeStillRuns) {
  const RoomModel model = radiator_room();
  Room room(model);
  const std::string id = this->start(living_room(), room.reading());
  ASSERT_TRUE(this->calibrate(id).ok);
  hub().max_file_bytes = 100;
  swing(room, model, 24.f, [&id] { return ended(id); });
  const AutotuneRun *run = hub().autotune(id);
  ASSERT_EQ(AutotuneState::SUCCEEDED, run->state());
  EXPECT_FALSE(run->persisted());
  EXPECT_EQ(run->new_gains().kp, hub().store().get(id)->pid.kp);
  EXPECT_EQ(0.6f, this->on_flash(id).pid.kp);
}

// The write is tried again, as a target's is, and a write that goes through says so.
TEST_F(Calibration, AResultTheFileDidNotTakeIsWrittenAgain) {
  const RoomModel model = radiator_room();
  Room room(model);
  const std::string id = this->start(living_room(), room.reading());
  ASSERT_TRUE(this->calibrate(id).ok);
  hub().max_file_bytes = 100;
  swing(room, model, 24.f, [&id] { return ended(id); });
  const AutotuneRun *run = hub().autotune(id);
  ASSERT_FALSE(run->persisted());
  hub().max_file_bytes = CONFIG_MAX_BYTES;
  hub().ms += 3000;
  hub().loop();
  EXPECT_EQ(run->new_gains().kp, this->on_flash(id).pid.kp);
  EXPECT_EQ(1u, this->on_flash(id).revision);
  EXPECT_TRUE(run->persisted());
}

TEST_F(Calibration, AResultTheFileDidNotTakeIsWrittenAtShutdown) {
  const RoomModel model = radiator_room();
  Room room(model);
  const std::string id = this->start(living_room(), room.reading());
  ASSERT_TRUE(this->calibrate(id).ok);
  hub().max_file_bytes = 100;
  swing(room, model, 24.f, [&id] { return ended(id); });
  hub().max_file_bytes = CONFIG_MAX_BYTES;
  hub().on_shutdown();
  EXPECT_EQ(hub().autotune(id)->new_gains().kp, this->on_flash(id).pid.kp);
}

// Failed again at the retry, the gains reach the file with the next change, and the warning goes.
TEST_F(Calibration, ALaterWriteOfTheThermostatClearsTheWarning) {
  const RoomModel model = radiator_room();
  Room room(model);
  const std::string id = this->start(living_room(), room.reading());
  ASSERT_TRUE(this->calibrate(id).ok);
  hub().max_file_bytes = 100;
  swing(room, model, 24.f, [&id] { return ended(id); });
  hub().ms += 3000;
  hub().loop();
  const AutotuneRun *run = hub().autotune(id);
  ASSERT_FALSE(run->persisted()) << "the retry failed too";
  hub().max_file_bytes = CONFIG_MAX_BYTES;
  ASSERT_TRUE(hub().set_setpoint(id, 22.f).ok);
  hub().ms += 3000;
  hub().loop();
  EXPECT_EQ(22.f, this->on_flash(id).setpoint);
  EXPECT_EQ(run->new_gains().kp, this->on_flash(id).pid.kp);
  EXPECT_TRUE(run->persisted());
}

// --- What a start is refused for ---

TEST_F(Calibration, OnlyARunningPidThermostatInHeatOrCoolStarts) {
  EXPECT_EQ(404, this->calibrate("attic").code);

  ClimateConfig boiler = draft("Boiler", "relay_2");
  boiler.kind = ControlKind::BANG_BANG;
  Result refused = this->calibrate(this->create(boiler).id);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("Only a PID thermostat can be calibrated", refused.error);

  ClimateConfig stopped = draft("Stopped", "relay_3");
  stopped.enabled = false;
  refused = this->calibrate(this->create(stopped).id);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("The thermostat is not running", refused.error);

  ClimateConfig off = living_room();
  off.mode = HubMode::OFF;
  const std::string id = this->start(off, 18.f);
  refused = this->calibrate(id);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("The thermostat is off: set it to heat or cool first", refused.error);
}

TEST_F(Calibration, ADirectionTheModeDoesNotDriveIsRefused) {
  ClimateConfig config = living_room();
  config.cool.relay_id = "relay_2";
  const std::string id = this->start(config, 18.f);
  Result refused = this->calibrate(id, AutotuneDirection::COOL);
  EXPECT_EQ(400, refused.code);
  EXPECT_EQ("direction 'cool' needs mode cool or heat_cool", refused.error);
  EXPECT_EQ(nullptr, hub().autotune(id));
}

TEST_F(Calibration, OneRunAtATimeAndNotOnAFault) {
  const std::string id = this->start(living_room(), 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  Result refused = this->calibrate(id);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("A calibration is already running", refused.error);

  ClimateConfig hot = draft("Sauna", "relay_2");
  hot.sensor_id = "floor";
  hot.safety.max_temperature = 25.f;
  const std::string sauna = this->create(hot).id;
  entities().floor.publish_state(30.f);
  hub().loop();
  refused = this->calibrate(sauna);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("The thermostat reports overtemp; calibrate it once that clears", refused.error);
}

// Its gains could not be written back.
TEST_F(Calibration, ANewerFirmwaresFileIsNotCalibrated) {
  const std::string id = this->create(living_room()).id;
  std::string file = read_file(this->file_of(id));
  const std::string ours = "\"version\":" + std::to_string(CONFIG_VERSION);
  file.replace(file.find(ours), ours.size(), "\"version\":" + std::to_string(CONFIG_VERSION + 1));
  write_file(this->file_of(id), file);
  this->reboot();
  Result refused = this->calibrate(id);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("A newer firmware wrote this thermostat; update the firmware to change it", refused.error);
}

// It waits for the first reading with its relays open, as the thermostat does.
TEST_F(Calibration, StartedBeforeAReadingItWaitsForOne) {
  const std::string id = this->create(living_room()).id;
  hub().ms += 1000;
  hub().loop();
  ASSERT_EQ(climate::CLIMATE_ACTION_IDLE, hub().entity_of(id)->action);
  ASSERT_TRUE(this->calibrate(id).ok);
  hub().ms += 1000;
  hub().loop();
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_FALSE(hub().autotune(id)->tuner().started());
  entities().room.publish_state(18.f);
  hub().loop();
  EXPECT_TRUE(entities().relay1.state);
}

// Inside the band the relay function starts off, whatever the PID was doing: upstream's choice.
TEST_F(Calibration, StartedInsideTheBandTheRelayStartsOpen) {
  ClimateConfig config = living_room();
  config.heat.period_s = 10.f;
  const std::string id = this->start(config, 20.9f);
  ASSERT_EQ(climate::CLIMATE_ACTION_HEATING, hub().entity_of(id)->action) << "the PID heats a little";
  const auto &seen = published(hub().entity_of(id));
  ASSERT_TRUE(this->calibrate(id).ok);
  EXPECT_EQ(std::vector<climate::ClimateAction>{climate::CLIMATE_ACTION_IDLE}, seen) << "Home Assistant hears of it";
  hold(20.9f, 10, 1);
  EXPECT_FALSE(entities().relay1.state);
}

// A new run replaces the last one's numbers.
TEST_F(Calibration, ANewRunReplacesTheLastOne) {
  const std::string id = this->start(living_room(), 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  ASSERT_TRUE(hub().cancel_autotune(id).ok);
  ASSERT_TRUE(this->calibrate(id, nullopt, AutotuneRule::PESSEN).ok);
  EXPECT_TRUE(hub().autotune(id)->running());
  EXPECT_EQ(AutotuneRule::PESSEN, hub().autotune(id)->rule());
}

TEST_F(Calibration, TheLogSaysWhichRunsCalibrate) {
  const std::string id = this->start(living_room(), 18.f);
  ASSERT_TRUE(this->calibrate(id).ok);
  LogCapture::instance().clear();
  hub().dump_config();
  EXPECT_TRUE(LogCapture::instance().has("running, calibrating"));
}

// --- The revision ---

// A form read before a calibration wrote new gains is refused; one read after goes through,
// and a Save keeps the revision.
TEST_F(Calibration, ASaveOfAnOlderDocumentIsRefused) {
  const RoomModel model = radiator_room();
  Room room(model);
  const std::string id = this->start(living_room(), room.reading());
  ClimateConfig form = *hub().store().get(id);
  ASSERT_EQ(0u, form.revision);
  ASSERT_TRUE(this->calibrate(id).ok);
  swing(room, model, 24.f, [&id] { return ended(id); });
  const float tuned = hub().store().get(id)->pid.kp;

  Result refused = hub().update(id, form, form.revision);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("The device changed this thermostat since it was read; reload it", refused.error);
  EXPECT_EQ(tuned, hub().store().get(id)->pid.kp);

  ClimateConfig fresh = *hub().store().get(id);
  fresh.name = "Lounge";
  ASSERT_TRUE(hub().update(id, fresh, 1u).ok);
  EXPECT_EQ(1u, hub().store().get(id)->revision);
  EXPECT_EQ(1u, this->on_flash(id).revision);
  ASSERT_TRUE(hub().update(id, fresh, 1u).ok) << "a Save does not move it";
  // A caller that sends none is not checked.
  form.name = "Lounge";
  ASSERT_TRUE(hub().update(id, form).ok);
  EXPECT_EQ(1u, hub().store().get(id)->revision);
  EXPECT_EQ(0.6f, hub().store().get(id)->pid.kp);
}

// The device writes a thermostat for other reasons too; none of them moves the revision.
TEST_F(Calibration, OnlyACalibrationMovesTheRevision) {
  const std::string id = this->start(living_room(), 18.f);
  ASSERT_TRUE(hub().set_setpoint(id, 22.f).ok);
  hub().ms += 3000;
  hub().loop();
  ASSERT_EQ(22.f, this->on_flash(id).setpoint) << "written";
  ASSERT_TRUE(hub().set_enabled(id, false).ok);
  ASSERT_TRUE(hub().set_enabled(id, true).ok);
  // Renamed at boot, since a YAML climate has its name now, and written back so.
  std::string file = read_file(this->file_of(id));
  file.replace(file.find("Living room"), 11, "Hall");
  write_file(this->file_of(id), file);
  this->reboot();
  ASSERT_EQ("Hall 2", this->on_flash(id).name);
  EXPECT_EQ(0u, this->on_flash(id).revision);
  EXPECT_EQ(0u, hub().store().get(id)->revision);
}

TEST_F(Calibration, ACreateStartsAtRevisionZero) {
  ClimateConfig config = living_room();
  config.revision = 9;
  const std::string id = this->create(config).id;
  EXPECT_EQ(0u, hub().store().get(id)->revision);
}

// --- The run on its own ---

namespace {

// Half-periods of ten hours and a swing of half a degree: Some Overshoot asks for a kd past the
// table's ceiling.
AutotuneRun slow_run(AutotuneRule rule) {
  AutotuneRun run(AutotuneDirection::HEAT, rule, PidGains{1.f, 2.f, 3.f}, 21.f, 0);
  const float readings[] = {21.f, 20.7f, 21.3f, 20.7f, 21.3f, 20.7f, 21.3f};
  for (size_t i = 0; i < std::size(readings); i++)
    run.feed(readings[i], static_cast<uint32_t>(i) * 36000000u);
  return run;
}

}  // namespace

TEST(AutotuneRun, AGainPastItsRangeIsClampedAndFlagged) {
  AutotuneRun run = slow_run(AutotuneRule::SOME_OVERSHOOT);
  ASSERT_TRUE(run.found());
  bool clamped = false;
  const PidGains gains = run.result(&clamped);
  EXPECT_TRUE(clamped);
  EXPECT_EQ(find_param("kd")->max, gains.kd);
  run.succeed(gains, clamped, true, 1);
  EXPECT_TRUE(run.clamped());

  AutotuneRun pi = slow_run(AutotuneRule::ZN_PI);
  pi.result(&clamped);
  EXPECT_FALSE(clamped) << "no kd to clamp";
}

TEST(AutotuneRun, TheFlagsWarnOnSuccess) {
  AutotuneRun run(AutotuneDirection::HEAT, AutotuneRule::ZN_PI, PidGains{}, 21.f, 0);
  // Two short half-periods and a long one, a small swing among large ones.
  const std::pair<uint32_t, float> readings[] = {{0, 21.f},    {60, 20.7f}, {120, 21.3f}, {180, 20.7f},
                                                 {900, 21.9f}, {960, 20.f}, {1020, 21.3f}};
  for (const auto &reading : readings)
    run.feed(reading.second, reading.first * 1000);
  ASSERT_TRUE(run.found());
  EXPECT_FALSE(run.asymmetric()) << "flags come with the result";
  run.succeed(PidGains{}, false, true, 1020000);
  EXPECT_TRUE(run.asymmetric());
  EXPECT_TRUE(run.uneven());
  EXPECT_EQ(AutotuneEnd::NONE, run.reason());
}

TEST(AutotuneRun, ItsLimitsCountFromTheStartAndTheLastSwitch) {
  AutotuneRun run(AutotuneDirection::HEAT, AutotuneRule::ZN_PI, PidGains{}, 21.f, 1000);
  EXPECT_EQ(AutotuneEnd::NONE, run.limit_reached(1000 + AUTOTUNE_STALL_MS - 1));
  EXPECT_EQ(AutotuneEnd::NO_SWITCH, run.limit_reached(1000 + AUTOTUNE_STALL_MS));
  run.feed(21.f, 1000);
  run.feed(20.f, 1000 + 5 * 3600000u);
  EXPECT_EQ(AutotuneEnd::NONE, run.limit_reached(1000 + 10 * 3600000u)) << "it switched at five hours";
  EXPECT_EQ(AutotuneEnd::TIMEOUT, run.limit_reached(1000 + AUTOTUNE_MAX_MS));
}

TEST(AutotuneRun, CrossingsPastTheCapAreNoise) {
  AutotuneRun run(AutotuneDirection::HEAT, AutotuneRule::ZN_PI, PidGains{}, 21.f, 1000);
  for (uint32_t n = 0; n < PidAutotuner::MAX_INTERVALS + 3; n++)
    run.feed(n % 2 == 0 ? 20.9f : 21.1f, 1000 + n * 10000);
  EXPECT_EQ(AutotuneEnd::NOISY, run.limit_reached(1000 + 3600000u));
}

}  // namespace esphome::climate_hub::testing
