// Create, rename, stop, delete and reload, against a real directory. Nothing can leave App, so
// a thermostat's entity is a slot of a pool registered at setup, and "gone" means hidden again.
#include "common.h"

namespace esphome::climate_hub::testing {

TEST_F(HubTest, CreateAssignsASlugWritesAFileAndStartsTheEntity) {
  Result result = this->create(draft("Living Room"));
  ASSERT_EQ("living-room", result.id);
  EXPECT_TRUE(result.persisted);
  EXPECT_TRUE(file_exists(this->file_of("living-room")));

  HubClimate *entity = hub().entity_of("living-room");
  ASSERT_NE(nullptr, entity);
  EXPECT_FALSE(entity->is_internal());
  EXPECT_EQ("Living Room", std::string(entity->get_name().c_str()));
  EXPECT_TRUE(hub().is_running("living-room"));
}

// The dashboard opens a blank editor at /climate/new, so no thermostat may have that id.
TEST_F(HubTest, NoThermostatGetsTheIdNew) {
  EXPECT_EQ("new-2", this->create(draft("New", "relay_1")).id);
  EXPECT_EQ("new-3", this->create(draft("new!", "relay_2")).id);
  EXPECT_EQ("newer", this->create(draft("Newer", "relay_3")).id);
  EXPECT_FALSE(file_exists(this->file_of("new")));
}

// Keying the file on the id, not the name, is what stops a second thermostat whose name
// slugifies alike from writing over the first.
TEST_F(HubTest, NamesThatSlugifyAlikeGetDistinctIds) {
  EXPECT_EQ("living-room", this->create(draft("Living Room", "relay_1")).id);
  EXPECT_EQ("living-room-2", this->create(draft("Living-Room", "relay_2")).id);
  EXPECT_EQ((std::vector<std::string>{"living-room-2.json", "living-room.json"}), list_dir(this->folder()));
}

// Files the loader left alone keep their ids. When they hold every id a name gives, the create
// is refused rather than written over one of them.
TEST_F(HubTest, ANameWhoseIdsAreAllTakenIsRefused) {
  for (unsigned n = 1; n <= 1000; n++)
    write_file(this->file_of(id_with_suffix("boiler", n)), "left alone");

  Result result = hub().create(draft("Boiler"));
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("Every id made from \"Boiler\" is taken by a file in the thermostat folder; choose another name",
            result.error);
  EXPECT_EQ(0u, hub().store().size());
  EXPECT_EQ(4u, hub().free_count());
  EXPECT_EQ(1000u, list_dir(this->folder()).size());
  EXPECT_EQ("left alone", read_file(this->file_of("boiler-1000")));
}

TEST_F(HubTest, TheEntityIsReachableThroughApp) {
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  const auto &climates = App.get_climates();
  EXPECT_NE(climates.end(), std::find(climates.begin(), climates.end(), entity))
      << "a thermostat has to be in App's climates to reach Home Assistant at all";
}

// The id, the file and the relay stay; the entity takes the new name, and with it a new object
// id: Home Assistant sees a new entity.
TEST_F(HubTest, RenameKeepsTheIdTheFileAndTheRelay) {
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  const RelayClaim *claim = hub().claim("relay_1");

  ASSERT_TRUE(hub().update("boiler", draft("Hot Water")).ok);
  EXPECT_EQ("Hot Water", hub().store().get("boiler")->name);
  EXPECT_TRUE(file_exists(this->file_of("boiler")));
  EXPECT_FALSE(file_exists(this->file_of("hot-water")));
  EXPECT_EQ(entity, hub().entity_of("boiler")) << "the same slot, renamed in place";
  EXPECT_EQ("Hot Water", std::string(entity->get_name().c_str()));
  EXPECT_EQ("hot_water", object_id(*entity));
  EXPECT_EQ(claim, hub().claim("relay_1"));
}

// Two thermostats must not share a name: compared as a person reads it, and as the entity id
// ESPHome derives from it, which Home Assistant keys the entity on.
TEST_F(HubTest, CreateRefusesANameAnotherThermostatUses) {
  this->create(draft("Living Room"));

  Result same = hub().create(draft("living   room", "relay_2"));
  EXPECT_FALSE(same.ok);
  EXPECT_EQ(409, same.code);
  EXPECT_EQ("\"living   room\" is already used by another thermostat", same.error);

  Result collides = hub().create(draft("Living_Room", "relay_2"));
  EXPECT_EQ(409, collides.code);
  EXPECT_EQ("\"Living_Room\" is too close to \"Living Room\": both are living_room to Home Assistant", collides.error);
  EXPECT_EQ(1u, hub().store().size());

  this->create(draft("Bathroom", "relay_2"));
}

TEST_F(HubTest, ObjectIdCollisionsAreRefused) {
  this->create(draft("Room 1"));
  for (const char *name : {"Room_1", "Room.1", "room 1", "ROOM-1"}) {
    Result result = hub().create(draft(name, "relay_2"));
    if (std::string(name) == "ROOM-1") {
      EXPECT_TRUE(result.ok) << "'-' survives into the id, so room-1 is its own";
      continue;
    }
    EXPECT_EQ(409, result.code) << name;
  }
}

TEST_F(HubTest, ARenameOntoATakenNameIsRefusedButItsOwnNameIsNot) {
  this->create(draft("Boiler", "relay_1"));
  this->create(draft("Kettle", "relay_2"));

  Result taken = hub().update("kettle", draft("BOILER", "relay_2"));
  EXPECT_EQ(409, taken.code);
  EXPECT_EQ("Kettle", hub().store().get("kettle")->name);

  ClimateConfig same = draft("Kettle", "relay_2");
  same.setpoint = 24;
  EXPECT_TRUE(hub().update("kettle", same).ok);
}

// The web server matches a climate by name, first come first served, so a YAML climate's name
// is not a thermostat's to take.
TEST_F(HubTest, AYamlClimatesNameIsRefused) {
  Result result = hub().create(draft("Hall"));
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("\"Hall\" is already used by another thermostat", result.error);
  EXPECT_EQ(409, hub().create(draft("HALL")).code) << "same entity id";
}

// internal: true hides a climate from the lists, not from the web server's name match.
TEST_F(HubTest, AnInternalYamlClimatesNameIsRefusedToo) {
  Result result = hub().create(draft("Cellar"));
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("\"Cellar\" is already used by another thermostat", result.error);
}

TEST_F(HubTest, TheNameRulesHold) {
  for (const char *bad : {"a/b", "a\\b", "", "Кухня"}) {
    Result result = hub().create(draft(bad));
    EXPECT_EQ(400, result.code) << bad;
  }
  Result trimmed = this->create(draft("  Porch  "));
  EXPECT_EQ("Porch", hub().store().get(trimmed.id)->name);
}

TEST_F(HubTest, DeleteRetiresTheEntityEverywhereItCanBeSeen) {
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  ASSERT_NE(nullptr, entity);

  Result removed = hub().remove("boiler");
  ASSERT_TRUE(removed.ok);
  EXPECT_TRUE(removed.persisted);

  EXPECT_EQ(nullptr, hub().store().get("boiler"));
  EXPECT_FALSE(hub().is_running("boiler"));
  EXPECT_FALSE(file_exists(this->file_of("boiler")));
  EXPECT_TRUE(entity->is_internal()) << "internal is what drops it from every listing";
  EXPECT_TRUE(entity->is_free());
  EXPECT_EQ(nullptr, App.get_climate_by_key(fnv1_hash("boiler"))) << "the API cannot address it";
  EXPECT_EQ("", hub().claimed_by("relay_1")) << "the relay is free for the next thermostat";
  EXPECT_EQ(404, hub().remove("boiler").code);
}

// The web server still answers a removed thermostat's name, as it does any internal entity: with
// the traits it had and a stopped state, and without a document behind it.
TEST_F(HubTest, AFreedSlotStillAnswersTraits) {
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  EXPECT_EQ(2u, entity->get_traits().get_supported_modes().size()) << "off + heat while running";

  ASSERT_TRUE(hub().remove("boiler").ok);
  EXPECT_EQ(entity, web_server_match("Boiler"));
  auto freed = entity->get_traits();
  EXPECT_EQ(2u, freed.get_supported_modes().size()) << "the traits it had";
  EXPECT_EQ(climate::CLIMATE_MODE_OFF, entity->mode);
  EXPECT_EQ(climate::CLIMATE_ACTION_OFF, entity->action);
  EXPECT_TRUE(std::isnan(entity->current_temperature));
}

// A delete that cannot unlink blanks the file, so the loader does not bring the thermostat back.
TEST_F(HubTest, ADeleteThatCannotUnlinkBlanksTheFile) {
  this->create(draft("Boiler"));
  hub().undeletable.push_back("boiler.json");
  Result removed = hub().remove("boiler");
  EXPECT_TRUE(removed.ok);
  EXPECT_TRUE(removed.persisted);
  EXPECT_EQ("", read_file(this->file_of("boiler")));

  this->reboot();
  EXPECT_EQ(nullptr, hub().store().get("boiler"));
  EXPECT_EQ("", read_file(this->file_of("boiler"))) << "refused, and left as it is";
  EXPECT_EQ("boiler-2", this->create(draft("Boiler")).id) << "a file nothing loaded still holds its id";
}

TEST_F(HubTest, TwoRunningThermostatsCannotHoldTheSameRelay) {
  this->create(draft("First", "relay_1"));
  EXPECT_EQ("first", hub().claimed_by("relay_1"));

  Result second = hub().create(draft("Second", "relay_1"));
  EXPECT_EQ(409, second.code);
  EXPECT_EQ("first", second.holder);
  EXPECT_EQ("\"Relay 1\" is already driven by \"First\"", second.error);
  EXPECT_EQ(nullptr, hub().store().get("second")) << "nothing written, nothing kept";
  EXPECT_EQ("first", hub().claimed_by("relay_1"));
}

// A claim is held by a running thermostat only, so two documents may name one relay as long as
// they take turns: a summer and a winter profile on one boiler.
TEST_F(HubTest, TwoThermostatsMayShareARelayByTakingTurns) {
  this->create(draft("First", "relay_1"));
  ClimateConfig second = draft("Second", "relay_1");
  second.enabled = false;
  this->create(second);
  EXPECT_EQ("first", hub().claimed_by("relay_1"));

  Result refused = hub().set_enabled("second", true);
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("first", refused.holder);
  EXPECT_FALSE(hub().store().get("second")->enabled);

  ASSERT_TRUE(hub().set_enabled("first", false).ok);
  ASSERT_TRUE(hub().set_enabled("second", true).ok);
  EXPECT_TRUE(hub().is_running("second"));
  EXPECT_EQ("second", hub().claimed_by("relay_1"));
}

// Taken over in one step: the holder is stopped, its flag stored, and the relay changes hands.
TEST_F(HubTest, TakeOverDisablesTheHolder) {
  this->create(draft("Winter", "relay_1"));
  ClimateConfig summer = draft("Summer", "relay_1");
  summer.enabled = false;
  this->create(summer);

  Result result = hub().set_enabled("summer", true, true);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.persisted);
  EXPECT_TRUE(hub().is_running("summer"));
  EXPECT_FALSE(hub().is_running("winter"));
  EXPECT_FALSE(hub().store().get("winter")->enabled);
  EXPECT_EQ("summer", hub().claimed_by("relay_1"));

  this->reboot();
  EXPECT_TRUE(hub().is_running("summer")) << "and both flags survive a reboot";
  EXPECT_FALSE(hub().is_running("winter"));
}

TEST_F(HubTest, DisableStopsTheEntityAndEnableBringsItBack) {
  this->create(draft("Boiler"));

  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  EXPECT_FALSE(hub().is_running("boiler"));
  EXPECT_EQ("", hub().claimed_by("relay_1"));
  EXPECT_FALSE(hub().store().get("boiler")->enabled);

  ASSERT_TRUE(hub().set_enabled("boiler", true).ok);
  EXPECT_TRUE(hub().is_running("boiler"));
  EXPECT_EQ("boiler", hub().claimed_by("relay_1"));
  EXPECT_EQ(404, hub().set_enabled("nope", true).code);
}

// The dashboard's +/- buttons take this path, so it works on a stopped thermostat too: that one
// has no entity to address.
TEST_F(HubTest, ASetpointMoveLandsWhetherTheThermostatRunsOrNot) {
  this->create(draft("Boiler"));

  ASSERT_TRUE(hub().set_setpoint("boiler", 24.5f).ok);
  EXPECT_FLOAT_EQ(24.5f, hub().store().get("boiler")->setpoint);
  EXPECT_FLOAT_EQ(24.5f, hub().entity_of("boiler")->target_temperature);

  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  ASSERT_TRUE(hub().set_setpoint("boiler", 19.f).ok);
  EXPECT_FLOAT_EQ(19.f, hub().store().get("boiler")->setpoint);
}

TEST_F(HubTest, ASetpointMoveIsHeldInsideTheVisualRange) {
  ClimateConfig config = draft("Boiler");
  config.visual.min_temperature = 10.f;
  config.visual.max_temperature = 30.f;
  this->create(config);

  ASSERT_TRUE(hub().set_setpoint("boiler", 99.f).ok);
  EXPECT_FLOAT_EQ(30.f, hub().store().get("boiler")->setpoint);
  ASSERT_TRUE(hub().set_setpoint("boiler", -99.f).ok);
  EXPECT_FLOAT_EQ(10.f, hub().store().get("boiler")->setpoint);

  EXPECT_EQ(404, hub().set_setpoint("no-such-controller", 20.f).code);
  EXPECT_EQ(400, hub().set_setpoint("boiler", NAN).code);
}

// Debounced, not skipped: the write is what survives the power cut the new target must survive.
TEST_F(HubTest, ASetpointMoveIsWrittenAfterTheDebounce) {
  this->create(draft("Boiler"));
  ASSERT_TRUE(hub().set_setpoint("boiler", 26.f).ok);
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find("\"setpoint\":21")) << "not yet";

  hub().ms += 2999;
  hub().loop();
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find("\"setpoint\":21")) << "still not";
  hub().ms += 1;
  hub().loop();
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find("\"setpoint\":26"));
}

TEST_F(HubTest, ASetpointMoveSurvivesAReload) {
  this->create(draft("Boiler"));
  ASSERT_TRUE(hub().set_setpoint("boiler", 26.f).ok);
  this->reboot();  // on_shutdown() flushes

  ASSERT_NE(nullptr, hub().store().get("boiler"));
  EXPECT_FLOAT_EQ(26.f, hub().store().get("boiler")->setpoint);
}

// An enabled thermostat whose sensor or relay is not on the device is saved enabled and waits,
// as one loaded at boot does; the answer says why it is not running.
TEST_F(HubTest, AnEnabledThermostatWithoutItsSensorOrRelayIsSavedAndWaits) {
  ClimateConfig ghost = draft("Ghost");
  ghost.sensor_id = "no_such_sensor";
  Result result = hub().create(ghost);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(200, result.code);
  EXPECT_EQ("ghost", result.id);
  EXPECT_EQ("not started: sensor 'no_such_sensor' not found", result.warning);
  EXPECT_TRUE(result.persisted);
  EXPECT_TRUE(hub().store().get("ghost")->enabled);
  EXPECT_FALSE(hub().is_running("ghost"));
  EXPECT_EQ(4u, hub().free_count()) << "it took no slot";
  EXPECT_EQ("", hub().claimed_by("relay_1")) << "nor its relay";

  result = hub().create(draft("Porch", "relay_9"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ("not started: relay 'relay_9' not found", result.warning);
  EXPECT_TRUE(hub().store().get("porch")->enabled);
  EXPECT_FALSE(hub().is_running("porch"));
  EXPECT_EQ((std::vector<std::string>{"ghost.json", "porch.json"}), list_dir(this->folder()));
  pass_resync_delay();
  EXPECT_EQ(0, hub().resyncs) << "nothing started, nothing to re-list";

  // The toggle still asks for what is there: it answers a 400 and changes nothing.
  result = hub().set_enabled("ghost", true);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("No sensor \"no_such_sensor\" on this device", result.error);
  EXPECT_TRUE(hub().store().get("ghost")->enabled);
  result = hub().set_enabled("porch", true);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("No switch \"relay_9\" on this device", result.error);

  this->reboot();
  for (const char *id : {"ghost", "porch"}) {
    EXPECT_TRUE(hub().store().get(id)->enabled) << id << " waits across a reboot too";
    EXPECT_FALSE(hub().is_running(id)) << id;
  }
}

// A Save that points a running thermostat at a missing relay stops it: it opens its relay, lets
// go of it and waits for the new one, saved and enabled.
TEST_F(HubTest, ASaveOntoAMissingRelayStopsTheThermostatAndKeepsIt) {
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  entities().relay1.turn_on();
  ASSERT_TRUE(entities().relay1.state);
  pass_resync_delay();
  hub().resyncs = 0;

  Result result = hub().update("boiler", draft("Boiler", "relay_9"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ("not started: relay 'relay_9' not found", result.warning);
  EXPECT_EQ("relay_9", hub().store().get("boiler")->heat.relay_id);
  EXPECT_TRUE(hub().store().get("boiler")->enabled);
  EXPECT_FALSE(hub().is_running("boiler"));
  EXPECT_TRUE(entity->is_internal());
  EXPECT_EQ("", hub().claimed_by("relay_1"));
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find("relay_9"));
  pass_resync_delay();
  EXPECT_EQ(1, hub().resyncs) << "its entity is gone from the lists";

  // Saved back onto a relay that is there, it runs again.
  result = hub().update("boiler", draft("Boiler"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ("", result.warning);
  EXPECT_TRUE(hub().is_running("boiler"));
}

// The same for a sensor: the relays it kept are opened and let go with it.
TEST_F(HubTest, ASaveOntoAMissingSensorStopsTheThermostatAndKeepsIt) {
  this->create(draft("Boiler"));
  ClimateConfig moved = draft("Boiler");
  moved.sensor_id = "no_such_sensor";
  Result result = hub().update("boiler", moved);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ("not started: sensor 'no_such_sensor' not found", result.warning);
  EXPECT_EQ("no_such_sensor", hub().store().get("boiler")->sensor_id);
  EXPECT_FALSE(hub().is_running("boiler"));
  EXPECT_EQ("", hub().claimed_by("relay_1"));
}

// A relay a running thermostat holds is still a 409, missing sensor or not: the holder decides.
TEST_F(HubTest, AHeldRelayStillRefusesASaveWhoseSensorIsMissing) {
  this->create(draft("Winter", "relay_1"));
  ClimateConfig summer = draft("Summer", "relay_1");
  summer.sensor_id = "no_such_sensor";
  Result result = hub().create(summer);
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("winter", result.holder);
  EXPECT_EQ(nullptr, hub().store().get("summer"));
}

// Checked before anything is taken over: a refused enable leaves the holder running.
TEST_F(HubTest, AMissingSensorRefusesATakeOverBeforeItStopsTheHolder) {
  this->create(draft("Winter", "relay_1"));
  ClimateConfig summer = draft("Summer", "relay_1");
  summer.enabled = false;
  summer.sensor_id = "no_such_sensor";
  this->create(summer);

  EXPECT_EQ(400, hub().set_enabled("summer", true, true).code);
  EXPECT_TRUE(hub().is_running("winter"));
  EXPECT_TRUE(hub().store().get("winter")->enabled);
}

TEST_F(HubTest, MaxControllersCapsCreation) {
  const char *relays[] = {"relay_1", "relay_2", "relay_3", "relay_1"};
  for (int i = 0; i < 4; i++) {
    ClimateConfig config = draft("C" + std::to_string(i), relays[i]);
    config.enabled = i < 3;
    this->create(config);
  }
  Result overflow = hub().create(draft("Overflow", "relay_3"));
  EXPECT_EQ(507, overflow.code);
  EXPECT_EQ("This device allows 4 thermostats; delete one to add another", overflow.error);
}

TEST_F(HubTest, ThermostatsReloadFromDiskWithTheirSettings) {
  ClimateConfig config = draft("Boiler");
  config.setpoint = 23.5f;
  config.pid.kp = 1.25f;
  this->create(config);

  this->reboot();
  const ClimateConfig *back = hub().store().get("boiler");
  ASSERT_NE(nullptr, back);
  EXPECT_EQ("Boiler", back->name);
  EXPECT_FLOAT_EQ(23.5f, back->setpoint);
  EXPECT_FLOAT_EQ(1.25f, back->pid.kp);
  EXPECT_TRUE(hub().is_running("boiler"));
}

// A thermostat that was switched off comes back switched off: a disabled heater that starts
// heating after a power cut is the failure this guards.
TEST_F(HubTest, ADisabledThermostatStaysDisabledAcrossAReload) {
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  this->create(config);

  this->reboot();
  EXPECT_NE(nullptr, hub().store().get("boiler"));
  EXPECT_FALSE(hub().is_running("boiler"));
}

// Nothing can be created below /proc, so the Save fails before a byte is written, and nothing
// else moves either.
TEST_F(HubTest, ASaveThatCannotReachItsFolderChangesNothing) {
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  this->create(config);
  const std::string before = read_file(this->file_of("boiler"));

  storage().path = "/proc/definitely-not-writable";
  ClimateConfig edited = draft("Boiler");
  edited.setpoint = 24.f;
  Result result = hub().update("boiler", edited);
  storage().path = this->base_;

  EXPECT_EQ(500, result.code);
  EXPECT_FALSE(result.persisted);
  EXPECT_FALSE(hub().is_running("boiler")) << "still disabled, so still not running";
  EXPECT_FALSE(hub().store().get("boiler")->enabled);
  EXPECT_FLOAT_EQ(21.f, hub().store().get("boiler")->setpoint);
  EXPECT_EQ("", hub().claimed_by("relay_1"));
  EXPECT_EQ(before, read_file(this->file_of("boiler")));
}

// A document built in C++ gets the ranges the file format has: the control loop converts
// these to milliseconds and sample counts, where 1e7 s or NaN would be undefined.
TEST_F(HubTest, NumbersFromCppAreClampedLikeTheFileFormat) {
  ClimateConfig config = draft("Boiler");
  config.safety.sensor_timeout_s = 1e7f;
  config.update_interval_s = NAN;
  config.heat.min_on_s = -5.f;
  config.heat.period_s = INFINITY;
  config.pid.output_samples = 2.6f;
  this->create(config);
  const ClimateConfig *stored = hub().store().get("boiler");
  EXPECT_FLOAT_EQ(86400.f, stored->safety.sensor_timeout_s);
  EXPECT_FLOAT_EQ(30.f, stored->update_interval_s) << "NaN takes the default";
  EXPECT_FLOAT_EQ(0.f, stored->heat.min_on_s);
  EXPECT_FLOAT_EQ(3600.f, stored->heat.period_s);
  EXPECT_FLOAT_EQ(3.f, stored->pid.output_samples);

  config.safety.sensor_timeout_s = NAN;
  config.pid.kp = -1.f;
  ASSERT_TRUE(hub().update("boiler", config).ok);
  EXPECT_FLOAT_EQ(300.f, stored->safety.sensor_timeout_s);
  EXPECT_FLOAT_EQ(0.f, stored->pid.kp);
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find("\"sensor_timeout_s\":300"));
  hub().loop();
}

TEST_F(HubTest, AFailedCreateLeavesNothingBehind) {
  storage().path = "/proc/definitely-not-writable";
  Result result = hub().create(draft("Boiler"));
  storage().path = this->base_;
  EXPECT_EQ(500, result.code);
  EXPECT_EQ(0u, hub().store().size());
  EXPECT_EQ(4u, hub().free_count());
}

TEST_F(HubTest, AnIdLongerThanAnyObjectIdIsA400) {
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  config.sensor_id = std::string(ENTITY_ID_MAX_LENGTH + 1, 'a');
  Result result = hub().create(config);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("sensor_id is longer than 120 characters", result.error);
  EXPECT_TRUE(list_dir(this->folder()).empty());
}

// The next boot refuses a file over the cap, so a Save that would write one writes nothing.
TEST_F(HubTest, AFileTheNextBootWouldRefuseIsNeverWritten) {
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  hub().max_file_bytes = 100;
  Result result = hub().create(config);
  EXPECT_EQ(413, result.code);
  EXPECT_EQ("The thermostat's file would be over 8 KiB", result.error);
  EXPECT_EQ(0u, hub().store().size());
  EXPECT_TRUE(list_dir(this->folder()).empty());

  hub().max_file_bytes = CONFIG_MAX_BYTES;
  this->create(config);
  hub().max_file_bytes = read_file(this->file_of("boiler")).size();
  ClimateConfig edited = config;
  edited.setpoint = 24.f;
  ASSERT_TRUE(hub().update("boiler", edited).ok) << "a file as large as the cap fits";
  const std::string before = read_file(this->file_of("boiler"));
  ClimateConfig longer = edited;
  longer.name = "Boiler room";
  result = hub().update("boiler", longer);
  EXPECT_EQ(413, result.code);
  EXPECT_EQ("Boiler", hub().store().get("boiler")->name);
  EXPECT_EQ(before, read_file(this->file_of("boiler")));
  EXPECT_EQ(std::vector<std::string>{"boiler.json"}, list_dir(this->folder()));
}

// A heap that runs out mid-document would leave a file with keys missing: nothing is written.
TEST_F(HubTest, ADocumentCutShortByTheHeapIsNeverWritten) {
  CountdownAllocator empty(0);
  hub().json_allocator = &empty;
  Result result = hub().create(draft("Boiler"));
  EXPECT_EQ(500, result.code);
  EXPECT_EQ("The thermostat's file could not be written", result.error);
  EXPECT_EQ(0u, hub().store().size());
  EXPECT_TRUE(list_dir(this->folder()).empty());

  hub().json_allocator = nullptr;
  this->create(draft("Boiler"));
  const std::string before = read_file(this->file_of("boiler"));
  CountdownAllocator short_heap(3);
  hub().json_allocator = &short_heap;
  ClimateConfig edited = draft("Boiler");
  edited.setpoint = 24.f;
  result = hub().update("boiler", edited);
  EXPECT_EQ(500, result.code);
  EXPECT_FALSE(result.persisted);
  EXPECT_FLOAT_EQ(21.f, hub().store().get("boiler")->setpoint);
  EXPECT_EQ(before, read_file(this->file_of("boiler")));
  hub().json_allocator = nullptr;
}

// The write goes beside the file and is renamed over it: nothing is left beside it after.
TEST_F(HubTest, AnAtomicWriteLeavesNoTemporaryFile) {
  this->create(draft("Boiler"));
  ASSERT_TRUE(hub().update("boiler", draft("Hot Water")).ok);
  EXPECT_EQ(std::vector<std::string>{"boiler.json"}, list_dir(this->folder()));
}

// A Save that keeps the relay keeps its claim: no off-on cycle that would skip min_off.
TEST_F(HubTest, ASaveWithTheSameRelayKeepsTheClaim) {
  ClimateConfig config = draft("Boiler");
  config.kind = ControlKind::BANG_BANG;
  entities().room.publish_state(15.f);
  this->create(config);
  hub().loop();
  ASSERT_TRUE(entities().relay1.state);
  const int writes = entities().relay1.writes;
  const RelayClaim *claim = hub().claim("relay_1");

  config.setpoint = 22.f;
  config.bang_bang.below = 1.f;
  ASSERT_TRUE(hub().update("boiler", config).ok);
  hub().ms += 100;
  hub().loop();
  EXPECT_EQ(claim, hub().claim("relay_1"));
  EXPECT_TRUE(entities().relay1.state);
  EXPECT_EQ(writes, entities().relay1.writes) << "the relay never moved";
}

// A Save onto another relay opens the old one and lets it go.
TEST_F(HubTest, ASaveOntoAnotherRelayReleasesTheOldOne) {
  ClimateConfig config = draft("Boiler");
  config.kind = ControlKind::BANG_BANG;
  entities().room.publish_state(15.f);
  this->create(config);
  hub().loop();
  ASSERT_TRUE(entities().relay1.state);

  config.heat.relay_id = "relay_2";
  ASSERT_TRUE(hub().update("boiler", config).ok);
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_EQ("", hub().claimed_by("relay_1"));
  EXPECT_EQ("boiler", hub().claimed_by("relay_2"));
  hub().loop();
  EXPECT_TRUE(entities().relay2.state);
}

// Several thermostats on one probe share one subscription: callbacks cannot be removed, so
// every extra one would live as long as the device.
TEST_F(HubTest, ASensorIsSubscribedOnce) {
  this->create(draft("First", "relay_1"));
  const size_t subscriptions = hub().sensor_subscriptions();
  this->create(draft("Second", "relay_2"));
  ASSERT_TRUE(hub().set_enabled("first", false).ok);
  ASSERT_TRUE(hub().set_enabled("first", true).ok);
  EXPECT_EQ(subscriptions, hub().sensor_subscriptions());

  entities().room.publish_state(19.25f);
  EXPECT_FLOAT_EQ(19.25f, hub().entity_of("first")->current_temperature);
  EXPECT_FLOAT_EQ(19.25f, hub().entity_of("second")->current_temperature);
}

// Home Assistant lists entities only when it connects, so a thermostat that appears, goes or is
// renamed has to make it reconnect: once for a burst of edits, and never for a target nudged
// from Home Assistant itself, or a dragged slider would take the device offline.
TEST_F(HubTest, HomeAssistantReconnectsOncePerBurstOfStructuralChanges) {
  this->create(draft("Boiler"));
  ASSERT_TRUE(hub().update("boiler", draft("Hot Water")).ok);
  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  ASSERT_TRUE(hub().set_enabled("boiler", true).ok);
  EXPECT_EQ(0, hub().resyncs) << "debounced";
  pass_resync_delay();
  EXPECT_EQ(1, hub().resyncs);

  ASSERT_TRUE(hub().set_setpoint("boiler", 25.f).ok);
  ClimateConfig gains = draft("Hot Water");
  gains.pid.kp = 2.f;
  ASSERT_TRUE(hub().update("boiler", gains).ok);
  pass_resync_delay();
  EXPECT_EQ(1, hub().resyncs) << "a target or new gains are no news to Home Assistant";

  ASSERT_TRUE(hub().remove("boiler").ok);
  pass_resync_delay();
  EXPECT_EQ(2, hub().resyncs);
}

// A thermostat created stopped brings no entity, so there is nothing to re-list.
TEST_F(HubTest, AStoppedCreateDoesNotReconnect) {
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  this->create(config);
  pass_resync_delay();
  EXPECT_EQ(0, hub().resyncs);
}

TEST_F(HubTest, ANaNTargetIsRefused) {
  ClimateConfig config = draft("Boiler");
  config.setpoint = NAN;
  Result result = hub().create(config);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("setpoint must be a number", result.error);
}

TEST_F(HubTest, AnUpdateNeedsAKnownIdAndAValidDocument) {
  EXPECT_EQ(404, hub().update("nope", draft("Nope")).code);

  this->create(draft("Boiler"));
  ClimateConfig broken = draft("Boiler");
  broken.sensor_id = "";
  Result result = hub().update("boiler", broken);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("sensor_id is required", result.error);
  EXPECT_EQ("room", hub().store().get("boiler")->sensor_id);
}

// A Save carries the enabled flag too, so it is also a way to stop or start a thermostat.
TEST_F(HubTest, ASaveThatSwitchesAThermostatOffStopsIt) {
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  pass_resync_delay();
  hub().resyncs = 0;

  ClimateConfig off = draft("Boiler");
  off.enabled = false;
  Result result = hub().update("boiler", off);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_FALSE(hub().is_running("boiler"));
  EXPECT_TRUE(entity->is_internal());
  EXPECT_EQ("", hub().claimed_by("relay_1"));
  pass_resync_delay();
  EXPECT_EQ(1, hub().resyncs);
}

TEST_F(HubTest, ASaveThatSwitchesAThermostatOnStartsIt) {
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  this->create(config);

  Result result = hub().update("boiler", draft("Boiler"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ("", result.warning);
  EXPECT_TRUE(hub().is_running("boiler"));
  EXPECT_EQ("boiler", hub().claimed_by("relay_1"));
  pass_resync_delay();
  EXPECT_EQ(1, hub().resyncs);
}

// Home Assistant lists a climate's name, modes and range: a Save that changes any of them makes
// it reconnect, and one that changes only state or tuning does not.
TEST_F(HubTest, HomeAssistantReconnectsForNewModesOrANewRange) {
  ClimateConfig config = draft("Boiler");
  this->create(config);
  pass_resync_delay();
  hub().resyncs = 0;

  config.mode = HubMode::OFF;
  config.bang_bang.below = 2.f;
  config.update_interval_s = 60.f;
  ASSERT_TRUE(hub().update("boiler", config).ok);
  pass_resync_delay();
  EXPECT_EQ(0, hub().resyncs) << "a mode, a band and an interval are no news";

  const std::pair<const char *, void (*)(ClimateConfig &)> changes[] = {
      {"a new minimum", [](ClimateConfig &c) { c.visual.min_temperature = 10.f; }},
      {"a new maximum", [](ClimateConfig &c) { c.visual.max_temperature = 30.f; }},
      {"a new step", [](ClimateConfig &c) { c.visual.step = 1.f; }},
      {"a cooling relay", [](ClimateConfig &c) { c.cool.relay_id = "relay_2"; }},
      {"no heating relay", [](ClimateConfig &c) { c.heat.relay_id = ""; }},
  };
  for (const auto &change : changes) {
    change.second(config);
    ASSERT_TRUE(hub().update("boiler", config).ok) << change.first;
    const int before = hub().resyncs;
    pass_resync_delay();
    EXPECT_EQ(before + 1, hub().resyncs) << change.first;
  }
}

// Asking for what already is changes nothing: no write, no restart, no reconnect.
TEST_F(HubTest, EnablingARunningOrDisablingAStoppedThermostatDoesNothing) {
  this->create(draft("Boiler", "relay_1"));
  ClimateConfig off = draft("Kettle", "relay_2");
  off.enabled = false;
  this->create(off);
  HubClimate *entity = hub().entity_of("boiler");
  pass_resync_delay();
  hub().resyncs = 0;
  // A trailing space that any rewrite would drop.
  for (const char *id : {"boiler", "kettle"})
    write_file(this->file_of(id), read_file(this->file_of(id)) + " ");

  EXPECT_TRUE(hub().set_enabled("boiler", true).ok);
  EXPECT_TRUE(hub().set_enabled("kettle", false).ok);
  EXPECT_EQ(entity, hub().entity_of("boiler"));
  for (const char *id : {"boiler", "kettle"})
    EXPECT_EQ(' ', read_file(this->file_of(id)).back()) << id << " was not rewritten";
  pass_resync_delay();
  EXPECT_EQ(0, hub().resyncs);
}

// The editor shows what a thermostat's sensor reads, whether the thermostat runs or not.
TEST_F(HubTest, ASensorReadingIsWhatTheSensorSaysNow) {
  EXPECT_TRUE(std::isnan(hub().sensor_reading("no_such_sensor")));
  EXPECT_TRUE(std::isnan(hub().sensor_reading("room"))) << "no reading yet";
  entities().room.publish_state(19.5f);
  EXPECT_FLOAT_EQ(19.5f, hub().sensor_reading("room"));
  entities().room.publish_state(-INFINITY);
  EXPECT_TRUE(std::isnan(hub().sensor_reading("room"))) << "-inf is no room temperature";
  entities().room.publish_state(INFINITY);
  EXPECT_TRUE(std::isnan(hub().sensor_reading("room"))) << "nor is +inf";
  entities().hidden.publish_state(30.f);
  EXPECT_TRUE(std::isnan(hub().sensor_reading("hidden"))) << "internal: not the editor's to show";
  entities().uptime.publish_state(3600.f);
  EXPECT_TRUE(std::isnan(hub().sensor_reading("uptime"))) << "not °C: no room temperature to show";
}

// The setpoints, the band and the cut-out are in °C: uptime in seconds would be read as degrees.
TEST_F(HubTest, AnEnabledThermostatNeedsASensorInCelsius) {
  ASSERT_TRUE(reports_celsius(entities().room)) << "test.yaml gives Room its unit";
  ClimateConfig config = draft("Boiler");
  config.sensor_id = "uptime";
  Result result = hub().create(config);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("\"Uptime\" reports s, not °C", result.error);
  config.sensor_id = "counter";
  result = hub().create(config);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("\"Counter\" reports no unit, not °C", result.error);
  EXPECT_TRUE(list_dir(this->folder()).empty()) << "nothing written";

  config.enabled = false;
  this->create(config);
  result = hub().set_enabled("boiler", true);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("\"Counter\" reports no unit, not °C", result.error);
  EXPECT_FALSE(hub().store().get("boiler")->enabled);

  config.enabled = true;
  config.sensor_id = "room";
  ASSERT_TRUE(hub().update("boiler", config).ok);
  config.sensor_id = "uptime";
  result = hub().update("boiler", config);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("\"Uptime\" reports s, not °C", result.error);
  EXPECT_EQ("room", hub().store().get("boiler")->sensor_id) << "the running one is left as it was";
  EXPECT_TRUE(hub().is_running("boiler"));
}

// internal: true keeps an entity to the firmware; to a thermostat it is not on the device.
TEST_F(HubTest, AnInternalSensorIsNeverFound) {
  ClimateConfig config = draft("Boiler");
  config.sensor_id = "hidden";
  Result result = hub().create(config);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ("not started: sensor 'hidden' not found", result.warning);
  EXPECT_FALSE(hub().is_running("boiler"));
}

// run_on_loop() is how an HTTP handler reaches the hub. With one task, as here, the job runs in
// place and its answer comes back.
TEST_F(HubTest, AJobHandedToTheHubRunsAndAnswers) {
  EXPECT_TRUE(hub().run_on_loop([]() { return true; }));
  EXPECT_FALSE(hub().run_on_loop([]() { return false; }));
  EXPECT_EQ(2, hub().loop_jobs);
}

// After dallas_scan (DATA) has made the probes a thermostat binds, before automations (DATA - 1)
// so a rule can one day name a thermostat.
TEST_F(HubTest, TheHubSetsUpBetweenTheProbesAndTheRules) {
  EXPECT_LT(hub().get_setup_priority(), setup_priority::DATA);
  EXPECT_GT(hub().get_setup_priority(), setup_priority::DATA - 1.f);
}

// The icon codegen registered rides in the entity fields, through every rename and hide.
TEST_F(HubTest, EveryThermostatWearsTheThermostatIcon) {
  char icon[MAX_ICON_LENGTH];
  EXPECT_STREQ("mdi:thermostat", hub().slot_entity(0)->get_icon_to(icon)) << "as registered";
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  EXPECT_STREQ("mdi:thermostat", entity->get_icon_to(icon)) << "shown";
  ASSERT_TRUE(hub().remove("boiler").ok);
  EXPECT_STREQ("mdi:thermostat", entity->get_icon_to(icon)) << "hidden";
}

TEST_F(HubTest, TheConfigDumpSaysWhatEachThermostatIsDoing) {
  this->create(draft("Boiler", "relay_1"));
  ClimateConfig off = draft("Kettle", "relay_2");
  off.enabled = false;
  this->create(off);
  write_file(this->file_of("attic"), R"({"version":1,"id":"attic","name":"Attic","kind":"bang_bang",)"
                                     R"("sensor_id":"gone","heat":{"relay_id":"relay_3"}})");
  this->reboot();

  LogCapture::instance().clear();
  hub().dump_config();
  const LogCapture &log = LogCapture::instance();
  EXPECT_TRUE(log.has("Folder: " + this->folder()));
  EXPECT_TRUE(log.has("Thermostats: 3 of 4"));
  EXPECT_TRUE(log.has("'Boiler' (boiler): pid, running"));
  EXPECT_TRUE(log.has("'Kettle' (kettle): pid, disabled"));
  EXPECT_TRUE(log.has("'Attic' (attic): bang_bang, not started"));
}

// A target that cannot be written stays live; the log says it will not survive a reboot.
TEST_F(HubTest, ATargetThatCannotBeWrittenStaysLive) {
  this->create(draft("Boiler"));
  const std::string before = read_file(this->file_of("boiler"));
  ASSERT_TRUE(hub().set_setpoint("boiler", 26.f).ok);

  LogCapture::instance().clear();
  storage().path = "/proc/definitely-not-writable";
  hub().ms += 3000;
  hub().loop();
  storage().path = this->base_;

  EXPECT_FALSE(hub().dirty("boiler")) << "one attempt per change";
  EXPECT_FLOAT_EQ(26.f, hub().entity_of("boiler")->target_temperature);
  EXPECT_EQ(before, read_file(this->file_of("boiler")));
  EXPECT_TRUE(LogCapture::instance().has("'boiler': the new target or mode was not written"));
}

// Written beside the file and renamed over it: when the rename is what fails, nothing is left
// beside it either. A directory in the file's place refuses the rename, for root too.
TEST_F(HubTest, AFailedRenameLeavesNoTemporaryFile) {
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  this->create(config);
  ASSERT_EQ(0, ::remove(this->file_of("boiler").c_str()));
  ASSERT_EQ(0, mkdir(this->file_of("boiler").c_str(), 0755));

  Result result = hub().update("boiler", config);
  EXPECT_EQ(500, result.code);
  EXPECT_FALSE(result.persisted);
  EXPECT_EQ(std::vector<std::string>{"boiler.json"}, list_dir(this->folder()));
}

// And when not even the temporary file opens, the document is left as it was.
TEST_F(HubTest, AWriteThatCannotStartChangesNothing) {
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  this->create(config);
  ASSERT_EQ(0, mkdir((this->file_of("boiler") + ".tmp").c_str(), 0755));

  config.setpoint = 25.f;
  Result result = hub().update("boiler", config);
  EXPECT_EQ(500, result.code);
  EXPECT_FLOAT_EQ(21.f, hub().store().get("boiler")->setpoint);
}

// When the partition refuses both the unlink and the blanking, the thermostat is gone from the
// running device but its file is whole: persisted says so, and the next boot brings it back.
TEST_F(HubTest, ADeleteThePartitionRefusesEntirelyIsNotPersisted) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes anywhere";
  this->create(draft("Boiler"));
  hub().undeletable.push_back("boiler.json");
  chmod(this->file_of("boiler").c_str(), 0444);
  Result removed = hub().remove("boiler");
  chmod(this->file_of("boiler").c_str(), 0644);

  EXPECT_TRUE(removed.ok);
  EXPECT_FALSE(removed.persisted);
  EXPECT_EQ(nullptr, hub().store().get("boiler"));
  this->reboot();
  EXPECT_TRUE(hub().is_running("boiler")) << "back from its file";
}

// A stopped thermostat has no entity: saving or removing it changes its file and nothing that
// Home Assistant lists.
TEST_F(HubTest, ASaveOrRemovalOfAStoppedThermostatTouchesOnlyItsFile) {
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  this->create(config);
  config.name = "Hot Water";
  config.setpoint = 24.f;

  ASSERT_TRUE(hub().update("boiler", config).ok);
  EXPECT_FALSE(hub().is_running("boiler"));
  EXPECT_EQ(4u, hub().free_count());
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find("\"name\":\"Hot Water\""));
  ASSERT_TRUE(hub().remove("boiler").ok);
  EXPECT_FALSE(file_exists(this->file_of("boiler")));
  pass_resync_delay();
  EXPECT_EQ(0, hub().resyncs);
}

// Enabled but not running, its sensor gone: switching it off stores the flag and nothing else.
TEST_F(HubTest, DisablingAThermostatThatCouldNotRunStoresTheFlag) {
  write_file(this->file_of("attic"), R"({"version":1,"id":"attic","name":"Attic","kind":"bang_bang",)"
                                     R"("sensor_id":"gone","heat":{"relay_id":"relay_1"}})");
  this->reboot();
  ASSERT_FALSE(hub().is_running("attic"));

  Result result = hub().set_enabled("attic", false);
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.persisted);
  EXPECT_FALSE(hub().store().get("attic")->enabled);
  EXPECT_NE(std::string::npos, read_file(this->file_of("attic")).find("\"enabled\":false"));
  pass_resync_delay();
  EXPECT_EQ(0, hub().resyncs);
}

// A take-over the partition cannot record still happens now; persisted says a reboot undoes it.
TEST_F(HubTest, ATakeOverThatCannotBeWrittenSaysSo) {
  this->create(draft("Winter", "relay_1"));
  ClimateConfig summer = draft("Summer", "relay_1");
  summer.enabled = false;
  this->create(summer);
  for (const char *id : {"winter", "summer"})
    ASSERT_EQ(0, mkdir((this->file_of(id) + ".tmp").c_str(), 0755));

  Result result = hub().set_enabled("summer", true, true);
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.persisted);
  EXPECT_TRUE(hub().is_running("summer"));
  EXPECT_FALSE(hub().is_running("winter"));

  this->reboot();
  EXPECT_TRUE(hub().is_running("winter")) << "the files still say winter";
  EXPECT_FALSE(hub().is_running("summer"));
}

// A setpoint that is already the target is no change, so nothing waits to be written.
TEST_F(HubTest, TheSameTargetOnAStoppedThermostatWritesNothing) {
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  this->create(config);
  ASSERT_TRUE(hub().set_setpoint("boiler", 21.f).ok);
  EXPECT_FALSE(hub().dirty("boiler"));
  ASSERT_TRUE(hub().set_setpoint("boiler", 22.f).ok);
  EXPECT_TRUE(hub().dirty("boiler"));
}

// Each probe's samples reach the thermostats on that probe and no other.
TEST_F(HubTest, ASampleReachesOnlyTheThermostatsOnItsProbe) {
  this->create(draft("Lounge", "relay_1"));
  ClimateConfig floor = draft("Floor Heating", "relay_2");
  floor.sensor_id = "floor";
  this->create(floor);

  entities().floor.publish_state(28.f);
  EXPECT_FLOAT_EQ(28.f, hub().entity_of("floor-heating")->current_temperature);
  EXPECT_TRUE(std::isnan(hub().entity_of("lounge")->current_temperature));
  entities().room.publish_state(19.f);
  EXPECT_FLOAT_EQ(19.f, hub().entity_of("lounge")->current_temperature);
  EXPECT_FLOAT_EQ(28.f, hub().entity_of("floor-heating")->current_temperature);
}

// Removed by hand over the file API first: the delete still stands, and nothing comes back.
TEST_F(HubTest, DeletingAThermostatWhoseFileIsGoneStands) {
  this->create(draft("Boiler"));
  ASSERT_EQ(0, ::remove(this->file_of("boiler").c_str()));
  Result removed = hub().remove("boiler");
  EXPECT_TRUE(removed.ok);
  EXPECT_TRUE(removed.persisted);
  EXPECT_EQ(nullptr, hub().store().get("boiler"));
}

// A full partition shows up when the file is closed, not when it is written: the Save fails
// there and leaves nothing beside the file. /dev/full plays the partition.
TEST_F(HubTest, AFullPartitionIsCaughtWhenTheFileCloses) {
  if (!file_exists("/dev/full"))
    GTEST_SKIP() << "no /dev/full";
  ClimateConfig config = draft("Boiler");
  config.enabled = false;
  this->create(config);
  const std::string before = read_file(this->file_of("boiler"));
  ASSERT_EQ(0, symlink("/dev/full", (this->file_of("boiler") + ".tmp").c_str()));

  config.setpoint = 25.f;
  Result result = hub().update("boiler", config);
  EXPECT_EQ(500, result.code);
  EXPECT_EQ(before, read_file(this->file_of("boiler")));
  EXPECT_EQ(std::vector<std::string>{"boiler.json"}, list_dir(this->folder()));
}

}  // namespace esphome::climate_hub::testing
