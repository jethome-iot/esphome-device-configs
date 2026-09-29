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

// A thermostat that is to run needs what it names to be there; one that is off may wait for it.
TEST_F(HubTest, AnEnabledThermostatNeedsItsSensorAndRelays) {
  ClimateConfig ghost = draft("Ghost");
  ghost.sensor_id = "no_such_sensor";
  Result result = hub().create(ghost);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("No sensor \"no_such_sensor\" on this device", result.error);

  result = hub().create(draft("Ghost", "relay_9"));
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("No switch \"relay_9\" on this device", result.error);
  EXPECT_TRUE(list_dir(this->folder()).empty()) << "nothing written";

  ghost.enabled = false;
  this->create(ghost);
  EXPECT_EQ(4u, hub().free_count()) << "kept, and it took no slot";
  result = hub().set_enabled("ghost", true);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("No sensor \"no_such_sensor\" on this device", result.error);
  EXPECT_FALSE(hub().store().get("ghost")->enabled);

  this->create(draft("Boiler"));
  ClimateConfig moved = draft("Boiler", "relay_9");
  result = hub().update("boiler", moved);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("relay_1", hub().store().get("boiler")->heat.relay_id) << "the running one is left as it was";
  EXPECT_TRUE(hub().is_running("boiler"));
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

// Nothing can be written below /proc, so the Save fails; the atomic write leaves the old file
// whole, and nothing else moves either.
TEST_F(HubTest, AFailedWriteChangesNothing) {
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

TEST_F(HubTest, AFailedCreateLeavesNothingBehind) {
  storage().path = "/proc/definitely-not-writable";
  Result result = hub().create(draft("Boiler"));
  storage().path = this->base_;
  EXPECT_EQ(500, result.code);
  EXPECT_EQ(0u, hub().store().size());
  EXPECT_EQ(4u, hub().free_count());
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

}  // namespace esphome::climate_hub::testing
