// What a schedule drives: a relay, a thermostat or a preset not on the device, one that comes
// back, one enabled schedule per target over every target the modes name, taking them over, and
// the listing a page picks a target from.
#include "common.h"

namespace esphome::schedules::testing {

class Targets : public EngineTest {
 protected:
  // A document on relay_1, off from Monday 00:00 on, as a file holds it.
  static std::string off_file(const char *id, const char *name, bool enabled = true, int version = 1,
                              const char *relays = R"({"type":"relay","id":"relay_1","value":{"type":"off"}})") {
    char buf[1024];
    snprintf(buf, sizeof(buf),
             R"({"version":%d,"id":"%s","name":"%s","enabled":%s,"modes":[{"id":"off","name":"Off","actions":[%s]}],)"
             R"("week":{"mon":[{"at":"00:00","mode":"off"}]}})",
             version, id, name, enabled ? "true" : "false", relays);
    return buf;
  }
  void write(const std::string &id, const std::string &json) {
    mkdir(this->folder().c_str(), 0755);
    write_file(this->file_of(id), json);
  }
  std::vector<IssueKind> kinds(const std::string &id) const {
    std::vector<IssueKind> out;
    for (const Issue &issue : this->engine->status(id).issues)
      out.push_back(issue.kind);
    return out;
  }
};

// The action is skipped, the rest of the list runs.
TEST_F(Targets, ARelayNotOnTheDeviceIsSkippedAndTheRestRuns) {
  this->boot(at_local(0, 9, 0));
  ScheduleConfig config;
  config.name = "Attic";
  config.add_mode("on", "On");
  config.add_relay("relay_7", true);
  config.add_relay("relay_1", true);
  config.add_point(0, 0);
  Result created = this->create(config);
  EXPECT_EQ("skips mode \"On\", action 1: relay \"relay_7\" is not on this device", created.warning);
  EXPECT_TRUE(LogCapture::instance().warned("'attic' saved but skips mode"));
  EXPECT_TRUE(LogCapture::instance().warned("Schedule 'Attic': relay 'relay_7' is not on this device; skipped"));
  EXPECT_EQ(std::vector<bool>{true}, this->e.relay1.writes);
  const ScheduleStatus status = this->engine->status("attic");
  EXPECT_EQ(ScheduleState::RUNNING, status.state);
  ASSERT_EQ(1u, status.issues.size());
  EXPECT_EQ(IssueKind::MISSING_RELAY, status.issues[0].kind);
  EXPECT_EQ("Relay \"relay_7\" is not on this device", status.issues[0].text);
  EXPECT_EQ(0, status.issues[0].mode);
  EXPECT_EQ(0, status.issues[0].action);
  EXPECT_EQ("relay_7", this->engine->target_name(Target{TargetType::RELAY, "relay_7"}));
}

TEST_F(Targets, AnInternalSwitchIsNoTarget) {
  this->boot(at_local(0, 9, 0));
  Result created = this->create(daily("Hidden", "hidden"));
  EXPECT_EQ("skips mode \"On\", action 1: relay \"hidden\" is not on this device", created.warning);
  EXPECT_TRUE(this->e.hidden.writes.empty());
}

// A disabled schedule is saved without one; the issues are listed all the same.
TEST_F(Targets, OnlyAnEnabledScheduleIsWarned) {
  this->boot(at_local(0, 9, 0));
  Result created = this->create(daily("Attic", "relay_7", false));
  EXPECT_EQ("", created.warning);
  EXPECT_EQ((std::vector<IssueKind>{IssueKind::MISSING_RELAY, IssueKind::MISSING_RELAY}), this->kinds("attic"));
}

TEST_F(Targets, AThermostatNotOnTheDeviceIsSkipped) {
  this->boot(at_local(0, 9, 0));
  Result created = this->create(thermostat_doc("Attic week", "attic", {off_at(0)}));
  EXPECT_EQ("skips mode \"Off\", action 1: thermostat \"attic\" is not on this device", created.warning);
  const ScheduleStatus status = this->engine->status("attic-week");
  EXPECT_EQ(ScheduleState::RUNNING, status.state);
  ASSERT_EQ(1u, status.issues.size());
  EXPECT_EQ(IssueKind::MISSING_THERMOSTAT, status.issues[0].kind);
  EXPECT_EQ("Thermostat \"attic\" is not on this device", status.issues[0].text);
  EXPECT_TRUE(
      LogCapture::instance().warned("Schedule 'Attic week': thermostat 'attic' is not on this device; skipped"));
}

// Back from a backup, the thermostat takes what the list passed, without the waits; the rest of
// the list goes on as it was.
TEST_F(Targets, AThermostatThatGoesAndComesBackTakesWhatTheListPassed) {
  const std::string id = this->add_thermostat(thermostat("Living room", "boiler"));
  this->boot(at_local(0, 7, 0));
  this->create(home());
  ASSERT_EQ("comfort", stored(id).active_preset);
  ASSERT_TRUE(this->engine->waiting("home"));
  climate_hub::ClimateConfig backup = stored(id);
  backup.active_preset.clear();
  backup.setpoint = 21.f;

  ASSERT_TRUE(hub().remove(id).ok);
  ScheduleStatus status = this->engine->status("home");
  EXPECT_EQ(ScheduleState::RUNNING, status.state) << "the relays still run";
  EXPECT_EQ((std::vector<IssueKind>{IssueKind::MISSING_THERMOSTAT, IssueKind::MISSING_THERMOSTAT}),
            this->kinds("home"));

  ASSERT_TRUE(hub().restore(backup).ok);
  EXPECT_TRUE(this->engine->status("home").issues.empty());
  EXPECT_EQ("", stored(id).active_preset) << "applied once the hub is done";
  const std::vector<bool> relay1 = this->e.relay1.writes;
  EXPECT_EQ(1u, this->engine->run_later());
  EXPECT_EQ("comfort", stored(id).active_preset);
  EXPECT_FLOAT_EQ(22.f, stored(id).setpoint);
  EXPECT_EQ(relay1, this->e.relay1.writes) << "only the thermostat's actions";
  EXPECT_TRUE(this->engine->waiting("home")) << "the list goes on where it was";
  this->engine->run(120);
  EXPECT_TRUE(this->e.relay2.state);
}

// One the list skipped because it was not there yet takes what it missed once it is created.
TEST_F(Targets, AThermostatCreatedAfterTheListSkippedItTakesWhatItMissed) {
  this->boot(at_local(0, 7, 0));
  this->create(home());
  EXPECT_TRUE(this->e.relay1.state) << "the rest ran";
  this->add_thermostat(thermostat("Living room", "boiler"));
  EXPECT_EQ(1u, this->engine->run_later());
  EXPECT_EQ("comfort", stored("living-room").active_preset);
}

// A thermostat the list has not reached yet needs nothing when it comes: the list gets to it.
TEST_F(Targets, AThermostatTheListHasNotReachedIsLeftToTheList) {
  this->boot(at_local(0, 9, 0));
  ScheduleConfig config;
  config.name = "Later";
  config.add_mode("day", "Day");
  config.add_wait(60);
  config.add_thermostat("living-room", ValueType::PRESET, "eco");
  config.add_point(wm(0, 8, 0), 0);
  this->create(config);
  this->add_thermostat(thermostat("Living room", "boiler"));
  EXPECT_EQ(0u, this->engine->run_later());
  this->engine->run(60);
  EXPECT_EQ("eco", stored("living-room").active_preset);
}

TEST_F(Targets, APresetThatGoesAndComesBack) {
  const std::string id = this->add_thermostat(thermostat());
  this->boot(at_local(0, 9, 0));
  this->create(thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "eco")}));
  climate_hub::ClimateConfig without = stored(id);
  without.presets.erase(without.presets.begin());
  ASSERT_TRUE(hub().update(id, without).ok);
  ScheduleStatus status = this->engine->status("heat");
  EXPECT_EQ(ScheduleState::RUNNING, status.state);
  ASSERT_EQ(1u, status.issues.size());
  EXPECT_EQ(IssueKind::MISSING_PRESET, status.issues[0].kind);
  EXPECT_EQ("Thermostat \"Living room\" has no preset \"eco\"", status.issues[0].text);

  // Renamed since: the issue says the name it has now.
  climate_hub::ClimateConfig renamed = stored(id);
  renamed.name = "Lounge";
  ASSERT_TRUE(hub().update(id, renamed).ok);
  EXPECT_EQ("Thermostat \"Lounge\" has no preset \"eco\"", this->engine->status("heat").issues[0].text);
  Result saved = this->engine->update("heat", thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "eco")}));
  EXPECT_EQ("skips mode \"eco\", action 1: thermostat \"Lounge\" has no preset \"eco\"", saved.warning);
  EXPECT_TRUE(LogCapture::instance().warned("Schedule 'Heat': thermostat 'living-room' has no preset 'eco'; skipped"));

  climate_hub::ClimateConfig with = stored(id);
  with.presets.push_back(preset("Eco", 17.f));
  ASSERT_TRUE(hub().update(id, with).ok);
  EXPECT_TRUE(this->engine->status("heat").issues.empty());
  EXPECT_EQ(1u, this->engine->run_later());
  EXPECT_EQ("eco", stored(id).active_preset);
  EXPECT_FLOAT_EQ(17.f, stored(id).setpoint);
}

// A change of a thermostat that took nothing away gives nothing back; a schedule that does not
// run hears of none.
TEST_F(Targets, AThermostatChangeThatLostNothingAppliesNothing) {
  const std::string id = this->add_thermostat(thermostat());
  this->boot(at_local(0, 9, 0));
  this->create(thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "eco")}));
  this->create(thermostat_doc("Spare", id.c_str(), {preset_at(wm(0, 6, 0), "comfort")}, false));
  ASSERT_TRUE(hub().apply_preset(id, "boost").ok);
  climate_hub::ClimateConfig more = stored(id);
  more.presets.push_back(preset("Night", 16.f));
  ASSERT_TRUE(hub().update(id, more).ok);
  EXPECT_EQ(0u, this->engine->run_later());
  EXPECT_EQ("boost", stored(id).active_preset) << "a hand change holds";
}

// The thermostat is back without a preset the list named: what can go in goes in, and the rest
// waits for the preset. A change that brings nothing back gives nothing; the preset does, and
// then every action on it the list passed applies again, in order.
TEST_F(Targets, AThermostatBackWithoutAPresetTakesWhatItCanAndTheRestOnceThePresetIs) {
  this->boot(at_local(0, 9, 0));
  ScheduleConfig config;
  config.name = "Heat";
  config.add_mode("day", "Day");
  config.add_thermostat("living-room", ValueType::TARGET, "", 23.f);
  config.add_thermostat("living-room", ValueType::PRESET, "night");
  config.add_point(0, 0);
  this->create(config);
  const std::string id = this->add_thermostat(thermostat());
  EXPECT_EQ(1u, this->engine->run_later());
  EXPECT_FLOAT_EQ(23.f, stored(id).setpoint) << "the target, which could go in";
  ASSERT_EQ(1u, this->kinds("heat").size());
  EXPECT_EQ(IssueKind::MISSING_PRESET, this->kinds("heat")[0]);

  ASSERT_TRUE(hub().set_setpoint(id, 19.f).ok);
  climate_hub::ClimateConfig dawn = stored(id);
  dawn.presets.push_back(preset("Dawn", 15.f));
  ASSERT_TRUE(hub().update(id, dawn).ok);
  EXPECT_EQ(0u, this->engine->run_later()) << "still no night";
  EXPECT_FLOAT_EQ(19.f, stored(id).setpoint) << "a hand change holds";

  LogCapture::instance().clear();
  climate_hub::ClimateConfig night = stored(id);
  night.presets.push_back(preset("Night", 16.f));
  ASSERT_TRUE(hub().update(id, night).ok);
  EXPECT_EQ(1u, this->engine->run_later());
  const auto &lines = LogCapture::instance().lines;
  auto line = [&lines](const char *needle) {
    return std::find_if(lines.begin(), lines.end(),
                        [needle](const std::string &l) { return l.find(needle) != std::string::npos; });
  };
  const auto target = line("Schedule 'Heat': thermostat 'living-room' target 23.0");
  const auto preset = line("Schedule 'Heat': thermostat 'living-room' preset 'night'");
  ASSERT_NE(lines.end(), target);
  ASSERT_NE(lines.end(), preset);
  EXPECT_LT(target, preset) << "in the list's order";
  EXPECT_EQ("night", stored(id).active_preset);
  EXPECT_FLOAT_EQ(16.f, stored(id).setpoint);
  EXPECT_TRUE(this->engine->status("heat").issues.empty());
  EXPECT_EQ(0u, this->engine->run_later());
}

// Actions on one thermostat on both sides of a wait, none of which reached it: once it is there,
// they apply in the list's order, at once, and nothing else of the list again.
TEST_F(Targets, ActionsOnAThermostatAcrossAWaitComeBackInOrder) {
  this->boot(at_local(0, 9, 0));
  ScheduleConfig config;
  config.name = "Heat";
  config.add_mode("day", "Day");
  config.add_thermostat("living-room", ValueType::TARGET, "", 20.f);
  config.add_wait(60);
  config.add_thermostat("living-room", ValueType::PRESET, "eco");
  config.add_relay("relay_1", true);
  config.add_point(0, 0);
  this->create(config);
  this->engine->run(60);
  ASSERT_EQ(std::vector<bool>{true}, this->e.relay1.writes);
  const std::string id = this->add_thermostat(thermostat());
  EXPECT_EQ(1u, this->engine->run_later());
  EXPECT_FLOAT_EQ(18.f, stored(id).setpoint) << "eco after the target, as the list has them";
  EXPECT_EQ("eco", stored(id).active_preset);
  EXPECT_EQ(std::vector<bool>{true}, this->e.relay1.writes);
  EXPECT_TRUE(this->engine->timers.empty()) << "no wait again";
}

// Every action that would be skipped, in every mode, mode by mode, and the warning names the first.
TEST_F(Targets, TheIssuesAreEveryActionOfEveryModeInOrder) {
  const std::string id = this->add_thermostat(thermostat());
  this->boot(at_local(0, 9, 0));
  ScheduleConfig doc;
  std::string error;
  ASSERT_TRUE(from_json(R"({"name":"Heat","modes":[)"
                        R"({"id":"a","name":"A","actions":[{"type":"relay","id":"relay_1","value":{"type":"on"}},)"
                        R"({"type":"wait","seconds":5},)"
                        R"({"type":"thermostat","id":"living-room","value":{"type":"preset","preset":"dawn"}}]},)"
                        R"({"id":"b","name":"B","actions":[{"type":"relay","id":"relay_9","value":{"type":"off"}},)"
                        R"({"type":"thermostat","id":"attic","value":{"type":"off"}}]}],)"
                        R"("week":{"mon":[{"at":"06:00","mode":"b"},{"at":"08:00","mode":"a"}]}})",
                        &doc, &error))
      << error;
  Result created = this->create(doc);
  EXPECT_EQ("skips mode \"A\", action 3: thermostat \"Living room\" has no preset \"dawn\"", created.warning);
  const ScheduleStatus status = this->engine->status("heat");
  ASSERT_EQ(3u, status.issues.size());
  EXPECT_EQ(IssueKind::MISSING_PRESET, status.issues[0].kind);
  EXPECT_EQ(0, status.issues[0].mode);
  EXPECT_EQ(2, status.issues[0].action);
  EXPECT_EQ(IssueKind::MISSING_RELAY, status.issues[1].kind);
  EXPECT_EQ(1, status.issues[1].mode);
  EXPECT_EQ(0, status.issues[1].action);
  EXPECT_EQ(IssueKind::MISSING_THERMOSTAT, status.issues[2].kind);
  EXPECT_EQ(1, status.issues[2].action);
  (void) id;
}

// Without a valid clock nothing runs, and that comes first; the issues are there all the same.
TEST_F(Targets, WaitingForTheClockComesFirst) {
  this->boot();
  this->create(daily("Attic", "relay_7"));
  ScheduleStatus status = this->engine->status("attic");
  EXPECT_EQ(ScheduleState::WAITING_FOR_CLOCK, status.state);
  EXPECT_EQ("Waiting for the clock", status.reason);
  EXPECT_EQ(2u, status.issues.size());
  this->engine->local = at_local(0, 9, 0);
  this->engine->tick();
  EXPECT_EQ(ScheduleState::RUNNING, this->state("attic"));
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

// The targets are every relay and thermostat any mode names; the refusal names the first that
// another enabled schedule drives, in the order the new one names them.
TEST_F(Targets, AScheduleSharingAnyTargetIsRefusedNamingTheFirstShared) {
  this->boot(at_local(0, 9, 0));
  this->create(home());
  ScheduleConfig lights;
  lights.name = "Lights";
  lights.add_mode("on", "On");
  lights.add_relay("boiler", true);
  lights.add_mode("off", "Off");
  lights.add_relay("relay_2", false);
  lights.add_relay("relay_1", false);
  lights.add_point(0, 0);
  Result refused = this->engine->create(lights);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("\"Home\" already drives Relay 2; switch it off first or take over", refused.error);
  EXPECT_EQ("home", refused.holder);
  // Off, which no point names, counts as much as On: its list is the schedule's too.
  lights.enabled = false;
  this->create(lights);
  EXPECT_EQ("home", this->engine->scheduled_by(TargetType::RELAY, "relay_2"));
  EXPECT_EQ("", this->engine->scheduled_by(TargetType::RELAY, "boiler")) << "a disabled one drives nothing";
}

TEST_F(Targets, ATargetThatIsNotThereStillHasOneSchedule) {
  this->boot(at_local(0, 9, 0));
  this->create(daily("Attic", "relay_7"));
  Result refused = this->engine->create(daily("Loft", "relay_7"));
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("\"Attic\" already drives relay_7; switch it off first or take over", refused.error);
}

TEST_F(Targets, ATakeOverStoresEveryOtherSharingATargetDisabled) {
  this->boot(at_local(0, 9, 0));
  this->create(daily("Porch"));
  this->create(relay_doc("Garden", "relay_2", {on_at(0)}));
  this->create(relay_doc("Shed", "boiler", {on_at(0)}));
  ScheduleConfig night = home("Night");
  night.enabled = false;
  this->create(night);

  Result refused = this->engine->set_enabled("night", true);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("\"Porch\" already drives Relay 1; switch it off first or take over", refused.error);
  Result taken = this->engine->set_enabled("night", true, true);
  ASSERT_TRUE(taken.ok) << taken.error;
  EXPECT_TRUE(taken.persisted);
  EXPECT_EQ((std::vector<std::string>{"garden", "porch"}), taken.stopped);
  EXPECT_EQ(ScheduleState::DISABLED, this->state("porch"));
  EXPECT_EQ(ScheduleState::DISABLED, this->state("garden"));
  EXPECT_EQ(ScheduleState::RUNNING, this->state("shed")) << "it shares nothing";
  EXPECT_EQ(ScheduleState::RUNNING, this->state("night"));
  EXPECT_NE(std::string::npos, read_file(this->file_of("porch")).find(R"("enabled":false)"));
  EXPECT_EQ("day", this->engine->list_mode("night")) << "the one taking over runs the mode in force";
  EXPECT_TRUE(this->engine->waiting("night"));

  refused = this->engine->set_enabled("porch", true);
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
  EXPECT_EQ("Schedule \"A\" drives Relay 1", status.reason);
  EXPECT_EQ("a", this->engine->scheduled_by(TargetType::RELAY, "relay_1"));

  // The one that drives can still be saved; the one that waits cannot be saved enabled.
  ASSERT_TRUE(this->engine->update("a", relay_doc("A", "relay_1", {on_at(wm(0, 8, 0))})).ok);
  Result refused = this->engine->update("b", relay_doc("B", "relay_1", {off_at(0)}));
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("a", refused.holder);

  Result stopped = this->engine->set_enabled("a", false);
  ASSERT_TRUE(stopped.ok);
  EXPECT_EQ(std::vector<std::string>{"b"}, stopped.started);
  EXPECT_FALSE(this->e.relay1.state) << "b runs its mode";
  EXPECT_EQ(ScheduleState::RUNNING, this->state("b"));
}

// Taken by id: one that waits drives none of its targets, so a later one has the others. Once
// the one in its way goes it runs, and the later one that shares a target with it waits.
TEST_F(Targets, OneThatWaitsLeavesItsOtherTargetsToTheNext) {
  this->write("a", file_doc("a", "A"));
  this->write("b", off_file("b", "B", true, 1,
                            R"({"type":"relay","id":"relay_2","value":{"type":"on"}},)"
                            R"({"type":"relay","id":"relay_1","value":{"type":"off"}})"));
  this->write("c", off_file("c", "C", true, 1, R"({"type":"relay","id":"relay_2","value":{"type":"off"}})"));
  this->boot(at_local(0, 9, 0));
  EXPECT_EQ(ScheduleState::RUNNING, this->state("a"));
  EXPECT_EQ(ScheduleState::CONFLICT, this->state("b"));
  EXPECT_EQ("Schedule \"A\" drives Relay 1", this->engine->status("b").reason) << "the one in its way, not C";
  EXPECT_EQ(ScheduleState::RUNNING, this->state("c"));
  EXPECT_EQ("c", this->engine->scheduled_by(TargetType::RELAY, "relay_2"));
  EXPECT_EQ(std::vector<bool>{false}, this->e.relay2.writes) << "c's list";

  // Each one that runs keeps the others off its targets; the 409 names the first shared.
  Result refused = this->engine->set_enabled("b", true);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("\"C\" already drives Relay 2; switch it off first or take over", refused.error);
  EXPECT_EQ("c", refused.holder);
  refused = this->engine->create(relay_doc("D", "relay_1", {on_at(0)}));
  EXPECT_EQ("a", refused.holder);

  this->e.relay2.writes.clear();
  Result stopped = this->engine->set_enabled("a", false);
  EXPECT_EQ(std::vector<std::string>{"b"}, stopped.started);
  EXPECT_EQ(ScheduleState::RUNNING, this->state("b"));
  EXPECT_EQ(ScheduleState::CONFLICT, this->state("c"));
  EXPECT_EQ("Schedule \"B\" drives Relay 2", this->engine->status("c").reason);
  EXPECT_EQ("b", this->engine->scheduled_by(TargetType::RELAY, "relay_2"));
  EXPECT_EQ(std::vector<bool>{true}, this->e.relay2.writes) << "b's list ran whole, nothing more from c";
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

// A Save onto other targets leaves the first as it is, runs on the new one, and lets the one
// that waited for the first drive it.
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
  EXPECT_EQ(std::vector<bool>{false}, this->e.relay1.writes) << "b's mode, nothing from a";
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

// A hub that dropped the thermostat without a word: the action says so and is skipped, and the
// thermostat takes it at the hub's next word about it.
TEST_F(Targets, AThermostatGoneWithoutAWordTakesItsActionAtTheHubsNextWord) {
  const std::string id = this->add_thermostat(thermostat());
  this->boot(at_local(0, 5, 0));
  this->create(thermostat_doc("Heat", id.c_str(), {preset_at(wm(0, 6, 0), "eco"), off_at(wm(6, 22, 0))}));
  climate_hub::ClimateHub *real = global_climate_hub;
  {
    // Its constructor makes it the firmware's hub; it knows no thermostat.
    climate_hub::ClimateHub empty;
    this->engine->stall(3600);
  }
  global_climate_hub = real;
  switch_hold::set_holder(real);
  EXPECT_TRUE(
      LogCapture::instance().warned("Schedule 'Heat': thermostat 'living-room' is not on this device; skipped"));
  EXPECT_NE("eco", stored(id).active_preset);
  // Nothing told the engine it is back, until the hub's next change of it.
  climate_hub::ClimateConfig more = stored(id);
  more.presets.push_back(preset("Night", 16.f));
  ASSERT_TRUE(hub().update(id, more).ok);
  EXPECT_EQ(1u, this->engine->run_later());
  EXPECT_EQ("eco", stored(id).active_preset);
}

// The hub refused the action: the log names the schedule, and the action applies at the hub's
// next word about the thermostat.
TEST_F(Targets, AnActionTheHubRefusesIsLoggedAndAppliesAtItsNextWord) {
  const std::string id = this->add_thermostat(thermostat());
  this->boot(at_local(0, 9, 0));
  hub().mark_failed();
  this->create(thermostat_doc("Heat plan", id.c_str(), {preset_at(wm(0, 6, 0), "eco")}));
  hub().reset_to_construction_state();
  EXPECT_TRUE(LogCapture::instance().warned(
      "Schedule 'Heat plan': thermostat 'living-room': Thermostat storage is not available"));
  EXPECT_NE("eco", stored(id).active_preset);
  climate_hub::ClimateConfig more = stored(id);
  more.presets.push_back(preset("Night", 16.f));
  ASSERT_TRUE(hub().update(id, more).ok);
  EXPECT_EQ(1u, this->engine->run_later());
  EXPECT_EQ("eco", stored(id).active_preset);
}

TEST_F(Targets, AFirmwareWhoseHubIsNotThereHasNoThermostats) {
  this->boot(at_local(0, 9, 0));
  climate_hub::ClimateHub *real = global_climate_hub;
  global_climate_hub = nullptr;
  Result created = this->engine->create(thermostat_doc("Attic week", "attic", {off_at(0)}));
  const std::vector<ThermostatTarget> thermostats = this->engine->thermostat_targets();
  const ScheduleStatus status = this->engine->status("attic-week");
  const std::string name = this->engine->target_name(Target{TargetType::THERMOSTAT, "attic"});
  global_climate_hub = real;
  EXPECT_EQ("skips mode \"Off\", action 1: this device has no thermostats", created.warning);
  EXPECT_TRUE(thermostats.empty());
  ASSERT_EQ(1u, status.issues.size());
  EXPECT_EQ(IssueKind::NO_THERMOSTATS, status.issues[0].kind);
  EXPECT_EQ("This device has no thermostats", status.issues[0].text);
  EXPECT_EQ("attic", name);
}

TEST(ScheduleStates, AreTheWordsTheApiSends) {
  EXPECT_STREQ("disabled", state_to_string(ScheduleState::DISABLED));
  EXPECT_STREQ("waiting_for_clock", state_to_string(ScheduleState::WAITING_FOR_CLOCK));
  EXPECT_STREQ("conflict", state_to_string(ScheduleState::CONFLICT));
  EXPECT_STREQ("running", state_to_string(ScheduleState::RUNNING));
  EXPECT_STREQ("missing_relay", issue_kind_to_string(IssueKind::MISSING_RELAY));
  EXPECT_STREQ("missing_thermostat", issue_kind_to_string(IssueKind::MISSING_THERMOSTAT));
  EXPECT_STREQ("missing_preset", issue_kind_to_string(IssueKind::MISSING_PRESET));
  EXPECT_STREQ("no_thermostats", issue_kind_to_string(IssueKind::NO_THERMOSTATS));
  EXPECT_STREQ("held", issue_kind_to_string(IssueKind::HELD));
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
  EXPECT_EQ("Relay 1", this->engine->target_name(Target{TargetType::RELAY, "relay_1"}));
  EXPECT_EQ("Living room", this->engine->target_name(Target{TargetType::THERMOSTAT, id}));
}

}  // namespace esphome::schedules::testing
