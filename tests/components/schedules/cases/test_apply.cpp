// What a value does to its target: a relay switched, or left to the thermostat that holds it
// until it lets go; a thermostat's preset, target or off, through the hub as a rule would.
#include "common.h"

namespace esphome::schedules::testing {

class Apply : public EngineTest {
 protected:
  // `config` created while the clock is not valid; the first valid tick reads `when`.
  void start(int64_t when, const ScheduleConfig &config) {
    this->boot();
    this->create(config);
    this->engine->local = when;
    this->engine->tick();
  }
};

TEST_F(Apply, ARelayTakesOnAndOff) {
  this->start(at_local(0, 9, 0), daily("Porch", "relay_2"));
  EXPECT_TRUE(this->e.relay2.state);
  this->engine->stall(11 * 3600);
  EXPECT_FALSE(this->e.relay2.state);
  EXPECT_EQ((std::vector<bool>{true, false}), this->e.relay2.writes);
  EXPECT_TRUE(this->e.relay1.writes.empty());
  EXPECT_TRUE(LogCapture::instance().has("Schedule 'Porch' (switch point): 'Relay 2' off"));
}

TEST_F(Apply, AHeldRelayIsLeftAloneAndItsHolderNamed) {
  this->add_thermostat(thermostat("Floor", "boiler"));
  ASSERT_EQ("Floor", switch_hold::holder(&this->e.boiler));
  const size_t before = this->e.boiler.writes.size();
  this->start(at_local(0, 9, 0), daily("Porch", "boiler"));
  EXPECT_EQ(before, this->e.boiler.writes.size());
  EXPECT_TRUE(LogCapture::instance().has("Schedule 'Porch' left 'Boiler' alone: thermostat 'Floor' drives it"));
  const ScheduleStatus status = this->engine->status("porch");
  EXPECT_EQ(ScheduleState::HELD, status.state);
  EXPECT_EQ("Floor", status.held_by);
  EXPECT_EQ("Thermostat \"Floor\" drives Boiler", status.reason);
}

// Decision 1: as an input binding's Follow, the relay takes the value in force once it is free.
TEST_F(Apply, AFreedRelayTakesTheValueInForce) {
  this->add_thermostat(thermostat("Floor", "boiler"));
  this->start(at_local(0, 9, 0), daily("Porch", "boiler"));
  ASSERT_TRUE(hub().set_enabled("floor", false).ok);
  EXPECT_FALSE(this->e.boiler.state) << "the hub opens the relay it lets go of";
  EXPECT_EQ(1u, this->engine->run_later()) << "the value waits for the hub to finish";
  EXPECT_TRUE(this->e.boiler.state);
  EXPECT_EQ(ScheduleState::RUNNING, this->state("porch"));
}

TEST_F(Apply, AFreedRelayGoesBackToAnOffInForceToo) {
  this->add_thermostat(thermostat("Floor", "boiler"));
  this->start(at_local(0, 21, 0), daily("Porch", "boiler"));
  this->e.boiler.writes.clear();
  ASSERT_TRUE(hub().set_enabled("floor", false).ok);
  this->engine->run_later();
  ASSERT_FALSE(this->e.boiler.writes.empty());
  EXPECT_FALSE(this->e.boiler.writes.back());
}

TEST_F(Apply, ARelayFreedThatNoScheduleDrivesQueuesNothing) {
  this->add_thermostat(thermostat("Floor", "boiler"));
  this->start(at_local(0, 9, 0), daily("Porch", "relay_1"));
  ASSERT_TRUE(hub().set_enabled("floor", false).ok);
  EXPECT_EQ(0u, this->engine->run_later());
}

TEST_F(Apply, ARelayTakenBackBeforeTheLoopStaysTheThermostats) {
  this->add_thermostat(thermostat("Floor", "boiler"));
  this->start(at_local(0, 9, 0), daily("Porch", "boiler"));
  ASSERT_TRUE(hub().set_enabled("floor", false).ok);
  ASSERT_TRUE(hub().set_enabled("floor", true).ok);
  const size_t before = this->e.boiler.writes.size();
  this->engine->run_later();
  EXPECT_EQ(before, this->e.boiler.writes.size());
  EXPECT_EQ(ScheduleState::HELD, this->state("porch"));
}

TEST_F(Apply, APresetThatKeepsTheModeTurnsAnOffThermostatOn) {
  const std::string id = this->add_thermostat(thermostat());
  ASSERT_TRUE(hub().turn_off(id).ok);
  this->start(at_local(0, 9, 0), thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "comfort")}));
  EXPECT_EQ(HubMode::HEAT, stored(id).mode);
  EXPECT_EQ("comfort", stored(id).active_preset);
  EXPECT_FLOAT_EQ(22.f, stored(id).setpoint);
}

TEST_F(Apply, APresetWithAModeOfItsOwnTakesThatMode) {
  const std::string id = this->add_thermostat(thermostat());
  ASSERT_TRUE(hub().turn_off(id).ok);
  this->start(at_local(0, 9, 0),
              thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "boost"), preset_at(wm(0, 10, 0), "away")}));
  EXPECT_EQ(HubMode::HEAT, stored(id).mode);
  EXPECT_FLOAT_EQ(25.f, stored(id).setpoint);
  this->engine->stall(3600);
  EXPECT_EQ(HubMode::OFF, stored(id).mode) << "away's own mode is off";
  EXPECT_EQ("away", stored(id).active_preset);
}

TEST_F(Apply, ATargetTurnsAnOffThermostatOn) {
  const std::string id = this->add_thermostat(thermostat());
  ASSERT_TRUE(hub().turn_off(id).ok);
  this->start(at_local(0, 9, 0),
              thermostat_doc("Heat", id.c_str(), {target_at(wm(0, 6, 0), 23.5f), target_at(wm(0, 10, 0), 19.f)}));
  EXPECT_EQ(HubMode::HEAT, stored(id).mode);
  EXPECT_FLOAT_EQ(23.5f, stored(id).setpoint);
  this->engine->stall(3600);
  EXPECT_FLOAT_EQ(19.f, stored(id).setpoint);
}

TEST_F(Apply, OffTurnsItOff) {
  const std::string id = this->add_thermostat(thermostat());
  this->start(at_local(0, 9, 0), thermostat_doc("Heat", id.c_str(), {off_at(wm(0, 6, 0))}));
  EXPECT_EQ(HubMode::OFF, stored(id).mode);
  EXPECT_TRUE(hub().is_running(id)) << "off is a mode, not a stop";
}

// The hub clamps, as for a rule; the schedule keeps what it was given.
TEST_F(Apply, ATargetOutsideTheRangeIsClampedByTheThermostat) {
  const std::string id = this->add_thermostat(thermostat());
  this->start(at_local(0, 9, 0), thermostat_doc("Heat", id.c_str(), {target_at(wm(0, 6, 0), 99.f)}));
  EXPECT_FLOAT_EQ(45.f, stored(id).setpoint);
  EXPECT_FLOAT_EQ(99.f, this->engine->get("heat")->points[0].target);
}

// Decision 7: a stopped thermostat takes the values into its document and stays stopped.
TEST_F(Apply, AStoppedThermostatTakesTheValuesAndStaysStopped) {
  const std::string id = this->add_thermostat(thermostat("Living room", "boiler", false));
  ASSERT_TRUE(hub().turn_off(id).ok);
  this->start(at_local(0, 9, 0), thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "comfort")}));
  EXPECT_EQ("comfort", stored(id).active_preset);
  EXPECT_FLOAT_EQ(22.f, stored(id).setpoint);
  EXPECT_EQ(HubMode::HEAT, stored(id).mode);
  EXPECT_FALSE(stored(id).enabled);
  EXPECT_FALSE(hub().is_running(id));
  const ScheduleStatus status = this->engine->status("heat");
  EXPECT_EQ(ScheduleState::RUNNING, status.state);
  EXPECT_EQ("Thermostat \"Living room\" is stopped; it starts in the scheduled values", status.note);
}

TEST_F(Apply, ARunningThermostatHasNoNote) {
  const std::string id = this->add_thermostat(thermostat());
  this->start(at_local(0, 9, 0), thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "eco")}));
  EXPECT_EQ("", this->engine->status("heat").note);
}

// Enabled but waiting for its sensor is not stopped: it runs, in the scheduled values, once the
// sensor comes.
TEST_F(Apply, AThermostatThatWaitsForItsSensorHasNoNote) {
  climate_hub::ClimateConfig waiting = thermostat();
  waiting.sensor_id = "nowhere";
  const std::string id = this->add_thermostat(waiting);
  ASSERT_FALSE(hub().is_running(id));
  this->start(at_local(0, 9, 0), thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "eco")}));
  EXPECT_EQ(ScheduleState::RUNNING, this->state("heat"));
  EXPECT_EQ("", this->engine->status("heat").note);
}

TEST_F(Apply, AScheduledChangeEndsACalibration) {
  climate_hub::ClimateConfig pid = thermostat();
  pid.kind = climate_hub::ControlKind::PID;
  const std::string id = this->add_thermostat(pid);
  this->start(at_local(0, 8, 59, 30),
              thermostat_doc("Heat", id.c_str(), {target_at(wm(0, 0, 0), 21.f), target_at(wm(0, 9, 0), 23.f)}));
  climate_hub::Result started = hub().start_autotune(id, nullopt, climate_hub::AutotuneRule::ZN_PI);
  ASSERT_TRUE(started.ok) << started.error;
  this->engine->run(60);
  const climate_hub::AutotuneRun *run = hub().autotune(id);
  ASSERT_NE(nullptr, run);
  EXPECT_FALSE(run->running());
  EXPECT_EQ(climate_hub::AutotuneEnd::TARGET_CHANGED, run->reason());
}

}  // namespace esphome::schedules::testing
