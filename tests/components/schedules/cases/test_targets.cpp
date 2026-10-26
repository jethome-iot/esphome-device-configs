// What a schedule drives: a target or a preset not on the device, one that comes back, one
// enabled schedule per target and taking it over, and the listing a page picks a target from.
#include "common.h"

namespace esphome::schedules::testing {

class Targets : public EngineTest {
 protected:
  // A document on relay_1, off from Monday 00:00 on, as a file holds it.
  static std::string off_file(const char *id, const char *name, bool enabled = true, int version = 1) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             R"({"version":%d,"id":"%s","name":"%s","enabled":%s,"target":{"type":"relay","id":"relay_1"},)"
             R"("week":{"mon":[{"at":"00:00","value":{"type":"off"}}]}})",
             version, id, name, enabled ? "true" : "false");
    return buf;
  }
  void write(const std::string &id, const std::string &json) {
    mkdir(this->folder().c_str(), 0755);
    write_file(this->file_of(id), json);
  }
};

TEST_F(Targets, ARelayNotOnTheDeviceIsSavedAndWaits) {
  this->boot(at_local(0, 9, 0));
  Result created = this->create(daily("Attic", "relay_7"));
  EXPECT_EQ("not running: relay \"relay_7\" is not on this device", created.warning);
  const ScheduleStatus status = this->engine->status("attic");
  EXPECT_EQ(ScheduleState::MISSING_TARGET, status.state);
  EXPECT_EQ("Relay \"relay_7\" is not on this device", status.reason);
  EXPECT_EQ("relay_7", this->engine->target_name(*this->engine->get("attic")));
  EXPECT_TRUE(LogCapture::instance().warned("'attic' saved but not running"));
}

TEST_F(Targets, AnInternalSwitchIsNoTarget) {
  this->boot(at_local(0, 9, 0));
  Result created = this->create(daily("Hidden", "hidden"));
  EXPECT_EQ("not running: relay \"hidden\" is not on this device", created.warning);
  EXPECT_TRUE(this->e.hidden.writes.empty());
}

TEST_F(Targets, AThermostatNotOnTheDeviceIsSavedAndWaits) {
  this->boot(at_local(0, 9, 0));
  Result created = this->create(thermostat_doc("Attic week", "attic", {off_at(0)}));
  EXPECT_EQ("not running: thermostat \"attic\" is not on this device", created.warning);
  EXPECT_EQ("Thermostat \"attic\" is not on this device", this->engine->status("attic-week").reason);
  EXPECT_EQ("attic", this->engine->target_name(*this->engine->get("attic-week")));
}

TEST_F(Targets, AThermostatThatGoesAndComesBackRunsAgain) {
  const std::string id = this->add_thermostat(thermostat());
  this->boot(at_local(0, 9, 0));
  this->create(thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "eco"), preset_at(wm(0, 18, 0), "comfort")}));
  ASSERT_EQ("eco", stored(id).active_preset);
  climate_hub::ClimateConfig backup = stored(id);
  backup.active_preset.clear();
  backup.setpoint = 21.f;

  ASSERT_TRUE(hub().remove(id).ok);
  ScheduleStatus status = this->engine->status("heat");
  EXPECT_EQ(ScheduleState::MISSING_TARGET, status.state);
  EXPECT_EQ("Thermostat \"living-room\" is not on this device", status.reason);

  ASSERT_TRUE(hub().restore(backup).ok);
  EXPECT_EQ(ScheduleState::RUNNING, this->state("heat"));
  EXPECT_EQ("", stored(id).active_preset) << "applied once the hub is done";
  EXPECT_EQ(1u, this->engine->run_later());
  EXPECT_EQ("eco", stored(id).active_preset);
  EXPECT_FLOAT_EQ(18.f, stored(id).setpoint);
}

TEST_F(Targets, APresetThatGoesAndComesBack) {
  const std::string id = this->add_thermostat(thermostat());
  this->boot(at_local(0, 9, 0));
  this->create(thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "eco")}));
  climate_hub::ClimateConfig without = stored(id);
  without.presets.erase(without.presets.begin());
  ASSERT_TRUE(hub().update(id, without).ok);
  ScheduleStatus status = this->engine->status("heat");
  EXPECT_EQ(ScheduleState::MISSING_PRESET, status.state);
  EXPECT_EQ("Thermostat \"Living room\" has no preset \"eco\"", status.reason);

  // Renamed since: the reason says the name it has now.
  climate_hub::ClimateConfig renamed = stored(id);
  renamed.name = "Lounge";
  ASSERT_TRUE(hub().update(id, renamed).ok);
  EXPECT_EQ("Thermostat \"Lounge\" has no preset \"eco\"", this->engine->status("heat").reason);
  Result saved = this->engine->update("heat", thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "eco")}));
  EXPECT_EQ("not running: thermostat \"Lounge\" has no preset \"eco\"", saved.warning);

  climate_hub::ClimateConfig with = stored(id);
  with.presets.push_back(preset("Eco", 17.f));
  ASSERT_TRUE(hub().update(id, with).ok);
  EXPECT_EQ(ScheduleState::RUNNING, this->state("heat"));
  this->engine->run_later();
  EXPECT_EQ("eco", stored(id).active_preset);
  EXPECT_FLOAT_EQ(17.f, stored(id).setpoint);
}

// The editor names the first missing in the week, whatever order the body listed them in.
TEST_F(Targets, TheFirstMissingPresetOfTheWeekIsNamed) {
  const std::string id = this->add_thermostat(thermostat());
  this->boot(at_local(0, 9, 0));
  ScheduleConfig doc;
  std::string error;
  ASSERT_TRUE(from_json(R"({"name":"Heat","target":{"type":"thermostat","id":")" + id +
                            R"("},"week":{"mon":[{"at":"08:00","value":{"type":"preset","preset":"night"}},)"
                            R"({"at":"06:00","value":{"type":"preset","preset":"dawn"}}]}})",
                        &doc, &error))
      << error;
  Result created = this->create(doc);
  EXPECT_EQ("not running: thermostat \"Living room\" has no preset \"dawn\"", created.warning);
  EXPECT_EQ(ScheduleState::MISSING_PRESET, this->state("heat"));
}

// Without a valid clock nothing runs, and that comes first: before a target that is missing.
TEST_F(Targets, WaitingForTheClockComesBeforeAMissingTarget) {
  this->boot();
  this->create(daily("Attic", "relay_7"));
  ScheduleStatus status = this->engine->status("attic");
  EXPECT_EQ(ScheduleState::WAITING_FOR_CLOCK, status.state);
  EXPECT_EQ("Waiting for the clock", status.reason);
  this->engine->local = at_local(0, 9, 0);
  this->engine->tick();
  EXPECT_EQ(ScheduleState::MISSING_TARGET, this->state("attic"));
}

TEST_F(Targets, OneEnabledSchedulePerTarget) {
  this->boot(at_local(0, 9, 0));
  this->create(daily("Porch"));
  const char *const DRIVEN = "\"Porch\" already drives Relay 1; switch it off first or take over";

  Result refused = this->engine->create(daily("Garden"));
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ(DRIVEN, refused.error);
  EXPECT_EQ("porch", refused.holder);

  this->create(daily("Garden", "relay_1", false));
  refused = this->engine->update("garden", daily("Garden"));
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ(DRIVEN, refused.error);
  refused = this->engine->set_enabled("garden", true);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ(DRIVEN, refused.error);
  EXPECT_FALSE(this->engine->get("garden")->enabled);
  ScheduleConfig back = daily("Garden");
  back.id = "garden";
  refused = this->engine->restore(back);
  EXPECT_EQ(409, refused.code);
  back.id = "garden-2";
  back.name = "Garden 2";
  refused = this->engine->restore(back);
  EXPECT_EQ(409, refused.code) << "an import onto a driven target";

  // A schedule on another relay is no one's business.
  this->create(daily("Shed", "relay_2"));
}

TEST_F(Targets, ATargetThatIsNotThereStillHasOneSchedule) {
  this->boot(at_local(0, 9, 0));
  this->create(daily("Attic", "relay_7"));
  Result refused = this->engine->create(daily("Loft", "relay_7"));
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("\"Attic\" already drives relay_7; switch it off first or take over", refused.error);
}

TEST_F(Targets, ATakeOverStoresTheOtherDisabled) {
  this->boot(at_local(0, 9, 0));
  this->create(daily("Porch"));
  this->create(relay_doc("Night", "relay_1", {off_at(0)}, false));
  ASSERT_TRUE(this->e.relay1.state);

  Result taken = this->engine->set_enabled("night", true, true);
  ASSERT_TRUE(taken.ok) << taken.error;
  EXPECT_TRUE(taken.persisted);
  EXPECT_EQ(std::vector<std::string>{"porch"}, taken.stopped);
  EXPECT_FALSE(this->e.relay1.state) << "the one taking over applies its value";
  EXPECT_EQ(ScheduleState::DISABLED, this->state("porch"));
  EXPECT_EQ(ScheduleState::RUNNING, this->state("night"));
  EXPECT_NE(std::string::npos, read_file(this->file_of("porch")).find(R"("enabled":false)"));

  Result refused = this->engine->set_enabled("porch", true);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("night", refused.holder);
}

TEST_F(Targets, TwoEnabledFromFilesTheFirstByIdDrives) {
  this->write("a", file_doc("a", "A"));
  this->write("b", off_file("b", "B"));
  this->boot(at_local(0, 9, 0));
  EXPECT_EQ(std::vector<bool>{true}, this->e.relay1.writes) << "a's on, b's off never";
  const ScheduleStatus status = this->engine->status("b");
  EXPECT_EQ(ScheduleState::CONFLICT, status.state);
  EXPECT_EQ("Schedule \"A\" drives this target", status.reason);
  EXPECT_EQ("a", this->engine->scheduled_by(TargetType::RELAY, "relay_1"));

  // The one that drives can still be saved; the one that waits cannot be saved enabled.
  ASSERT_TRUE(this->engine->update("a", relay_doc("A", "relay_1", {on_at(wm(0, 8, 0))})).ok);
  Result refused = this->engine->update("b", relay_doc("B", "relay_1", {off_at(0)}));
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("a", refused.holder);

  Result stopped = this->engine->set_enabled("a", false);
  ASSERT_TRUE(stopped.ok);
  EXPECT_EQ(std::vector<std::string>{"b"}, stopped.started);
  EXPECT_FALSE(this->e.relay1.state) << "b applies its value";
  EXPECT_EQ(ScheduleState::RUNNING, this->state("b"));
}

TEST_F(Targets, RemovingTheOneThatDrivesStartsTheNext) {
  this->write("a", file_doc("a", "A"));
  this->write("b", off_file("b", "B"));
  this->boot(at_local(0, 9, 0));
  Result removed = this->engine->remove("a");
  ASSERT_TRUE(removed.ok);
  EXPECT_EQ(std::vector<std::string>{"b"}, removed.started);
  EXPECT_FALSE(this->e.relay1.state);
}

TEST_F(Targets, AWaitingOneMovedElsewhereRunsThere) {
  this->write("a", file_doc("a", "A"));
  this->write("b", off_file("b", "B"));
  this->boot(at_local(0, 9, 0));
  this->e.relay2.publish_state(true);
  Result moved = this->engine->update("b", relay_doc("B", "relay_2", {off_at(0)}));
  ASSERT_TRUE(moved.ok) << moved.error;
  EXPECT_FALSE(this->e.relay2.state);
  EXPECT_TRUE(moved.started.empty());
}

// A Save onto another target leaves the first as it is, applies to the new one, and lets the
// one that waited for the first drive it.
TEST_F(Targets, ASaveOntoAnotherTargetLeavesTheFirstAsItIs) {
  this->write("a", file_doc("a", "A"));
  this->write("b", off_file("b", "B"));
  this->boot(at_local(0, 9, 0));
  ASSERT_TRUE(this->e.relay1.state);
  this->e.relay1.writes.clear();
  Result moved = this->engine->update("a", daily("A", "relay_2"));
  ASSERT_TRUE(moved.ok) << moved.error;
  EXPECT_TRUE(this->e.relay2.state);
  EXPECT_EQ(std::vector<std::string>{"b"}, moved.started);
  EXPECT_EQ(std::vector<bool>{false}, this->e.relay1.writes) << "b's value, nothing from a";
}

// A newer firmware's file cannot record the take-over: the others' files keep theirs.
TEST_F(Targets, ATakeOverByANewerFileLastsUntilTheReboot) {
  this->write("a", file_doc("a", "A"));
  this->write("b", off_file("b", "B", false, 2));
  this->boot(at_local(0, 9, 0));
  const std::string before = read_file(this->file_of("a"));
  Result taken = this->engine->set_enabled("b", true, true);
  ASSERT_TRUE(taken.ok) << taken.error;
  EXPECT_FALSE(taken.persisted);
  EXPECT_EQ(std::vector<std::string>{"a"}, taken.stopped);
  EXPECT_EQ(before, read_file(this->file_of("a")));
  EXPECT_FALSE(this->engine->get("a")->enabled);
  EXPECT_FALSE(this->e.relay1.state);
}

TEST_F(Targets, TheListingSaysWhoHoldsAndWhoSchedules) {
  const std::string id = this->add_thermostat(thermostat("Living room", "boiler"));
  this->boot(at_local(0, 9, 0));
  this->create(daily("Porch"));
  this->create(thermostat_doc("Heat plan", id.c_str(), {preset_at(0, "eco")}));
  this->create(thermostat_doc("Spare", id.c_str(), {off_at(0)}, false));

  const std::vector<RelayTarget> relays = this->engine->relay_targets();
  ASSERT_EQ(3u, relays.size()) << "the internal switch is no target";
  EXPECT_EQ("relay_1", relays[0].object_id);
  EXPECT_EQ("Relay 1", relays[0].name);
  EXPECT_EQ("", relays[0].held_by);
  EXPECT_EQ("porch", relays[0].scheduled_by);
  EXPECT_EQ("", relays[1].scheduled_by);
  EXPECT_EQ("boiler", relays[2].object_id);
  EXPECT_EQ("Living room", relays[2].held_by);

  const std::vector<ThermostatTarget> thermostats = this->engine->thermostat_targets();
  ASSERT_EQ(1u, thermostats.size());
  const ThermostatTarget &row = thermostats[0];
  EXPECT_EQ(id, row.id);
  EXPECT_EQ("Living room", row.name);
  EXPECT_TRUE(row.enabled);
  EXPECT_TRUE(row.running);
  EXPECT_EQ("heat", row.mode);
  EXPECT_FLOAT_EQ(5.f, row.min_temperature);
  EXPECT_FLOAT_EQ(45.f, row.max_temperature);
  EXPECT_FLOAT_EQ(0.5f, row.step);
  EXPECT_EQ("heat-plan", row.scheduled_by) << "the enabled one, not the spare";
  ASSERT_EQ(4u, row.presets.size());
  EXPECT_EQ("eco", row.presets[0].key);
  EXPECT_EQ("Eco", row.presets[0].name);
  EXPECT_FLOAT_EQ(18.f, row.presets[0].setpoint);
  EXPECT_EQ("keep", row.presets[0].mode);
  EXPECT_EQ("off", row.presets[2].mode);
  EXPECT_EQ("heat", row.presets[3].mode);
}

// A hub that dropped the thermostat without a word: the next point finds it gone and says so.
TEST_F(Targets, AThermostatGoneWithoutAWordIsFoundMissingAtTheNextPoint) {
  const std::string id = this->add_thermostat(thermostat());
  this->boot(at_local(0, 5, 0));
  this->create(thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "eco")}));
  climate_hub::ClimateHub *real = global_climate_hub;
  {
    // Its constructor makes it the firmware's hub; it knows no thermostat.
    climate_hub::ClimateHub empty;
    this->engine->stall(3600);
  }
  global_climate_hub = real;
  switch_hold::set_holder(real);
  EXPECT_TRUE(LogCapture::instance().warned("Schedule 'heat': thermostat 'living-room': Thermostat not found"));
  // The real hub has it again, but nothing told the engine: it says what it found last.
  const ScheduleStatus status = this->engine->status("heat");
  EXPECT_EQ(ScheduleState::MISSING_TARGET, status.state);
  EXPECT_EQ("Thermostat \"living-room\" is not on this device", status.reason);
}

TEST_F(Targets, AFirmwareWhoseHubIsNotThereHasNoThermostats) {
  this->boot(at_local(0, 9, 0));
  climate_hub::ClimateHub *real = global_climate_hub;
  global_climate_hub = nullptr;
  Result created = this->engine->create(thermostat_doc("Attic week", "attic", {off_at(0)}));
  const std::vector<ThermostatTarget> thermostats = this->engine->thermostat_targets();
  const std::string name = this->engine->target_name(*this->engine->get("attic-week"));
  global_climate_hub = real;
  EXPECT_EQ("not running: this device has no thermostats", created.warning);
  EXPECT_TRUE(thermostats.empty());
  EXPECT_EQ("attic", name);
}

TEST(ScheduleStates, AreTheWordsTheApiSends) {
  EXPECT_STREQ("disabled", state_to_string(ScheduleState::DISABLED));
  EXPECT_STREQ("waiting_for_clock", state_to_string(ScheduleState::WAITING_FOR_CLOCK));
  EXPECT_STREQ("missing_target", state_to_string(ScheduleState::MISSING_TARGET));
  EXPECT_STREQ("missing_preset", state_to_string(ScheduleState::MISSING_PRESET));
  EXPECT_STREQ("conflict", state_to_string(ScheduleState::CONFLICT));
  EXPECT_STREQ("held", state_to_string(ScheduleState::HELD));
  EXPECT_STREQ("running", state_to_string(ScheduleState::RUNNING));
}

// The seams the other cases replace: no clock source, no valid clock; the loop's own count.
TEST(ScheduleClocks, WithoutAClockSourceTheClockIsNeverValid) {
  struct Plain : ScheduleEngine {
    uint64_t mono() { return this->mono_ms_(); }
  } plain;
  std::string day;
  std::string time;
  EXPECT_FALSE(plain.clock_text(&day, &time));
  EXPECT_LE(plain.mono(), millis_64());
}

TEST_F(Targets, TargetNamesAreTheNamesPeopleKnow) {
  const std::string id = this->add_thermostat(thermostat());
  this->boot();
  EXPECT_EQ("Relay 1", this->engine->target_name(daily()));
  EXPECT_EQ("Living room", this->engine->target_name(thermostat_doc("Heat", id.c_str(), {off_at(0)})));
}

}  // namespace esphome::schedules::testing
