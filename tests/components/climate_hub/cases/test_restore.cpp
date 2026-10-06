// restore(): a thermostat brought back under the id its document names, as a backup holds it,
// with the keys its presets had and the preset that was active, so the rules naming them still
// find them. Refused as create() and update() refuse.
#include "common.h"

namespace esphome::climate_hub::testing {
namespace {

PresetConfig preset(const char *key, const char *name, float setpoint, optional<HubMode> mode = nullopt) {
  PresetConfig p;
  p.key = key;
  p.name = name;
  p.setpoint = setpoint;
  p.mode = mode;
  return p;
}

// What a backup holds: an id that is not its name's slug, keys no create would make, and the
// preset picked last with a target set by hand since.
ClimateConfig backup(const char *id = "lounge", const char *relay = "relay_1") {
  ClimateConfig c = draft("Living Room", relay);
  c.id = id;
  c.setpoint = 20.5f;
  c.presets = {preset("day-time", "Comfort", 22.f), preset("night-2", "Night", 19.f, HubMode::HEAT)};
  c.active_preset = "night-2";
  return c;
}

class Restore : public HubTest {
 protected:
  void quiet() {
    pass_resync_delay();
    hub().resyncs = 0;
  }

  // The file `id` holds, read back as the next boot reads it.
  ClimateConfig on_flash(const std::string &id) {
    ClimateConfig config;
    std::string error;
    EXPECT_TRUE(from_json(read_file(this->file_of(id)), &config, &error)) << error;
    return config;
  }
};

}  // namespace

TEST_F(Restore, CreatesTheThermostatUnderTheIdItBrings) {
  Result result = hub().restore(backup());
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ("lounge", result.id);
  EXPECT_EQ("", result.warning);
  EXPECT_EQ(std::vector<std::string>{"lounge.json"}, list_dir(this->folder()));
  ASSERT_TRUE(hub().is_running("lounge"));
  EXPECT_EQ("Living Room", std::string(hub().entity_of("lounge")->get_name().c_str()));
  EXPECT_EQ("lounge", hub().claimed_by("relay_1"));
  pass_resync_delay();
  EXPECT_EQ(1, hub().resyncs) << "a new entity for Home Assistant to list";
}

// A create would make "comfort" and "night", and pick none; the rules name these.
TEST_F(Restore, KeepsThePresetsKeysTheActivePresetAndTheTarget) {
  ASSERT_TRUE(hub().restore(backup()).ok);
  for (const ClimateConfig &config : {*hub().store().get("lounge"), this->on_flash("lounge")}) {
    ASSERT_EQ(2u, config.presets.size());
    EXPECT_EQ("day-time", config.presets[0].key);
    EXPECT_EQ("night-2", config.presets[1].key);
    EXPECT_EQ("night-2", config.active_preset);
    EXPECT_FLOAT_EQ(20.5f, config.setpoint) << "the target set by hand since the pick stays";
  }
  EXPECT_FLOAT_EQ(20.5f, hub().entity_of("lounge")->target_temperature);

  this->reboot();
  const ClimateConfig *loaded = hub().store().get("lounge");
  ASSERT_NE(nullptr, loaded);
  EXPECT_EQ("night-2", loaded->active_preset);
  EXPECT_TRUE(hub().is_running("lounge"));
}

// As the loader reads a file: a preset without a key gets one, an active preset none has is none.
TEST_F(Restore, GivesAKeyWhereNoneCameAndDropsAnActivePresetNoPresetHas) {
  ClimateConfig doc = backup();
  doc.presets.push_back(preset("", "Day Time", 21.f));
  doc.active_preset = "gone";
  ASSERT_TRUE(hub().restore(doc).ok);
  const ClimateConfig *stored = hub().store().get("lounge");
  EXPECT_EQ("day-time-2", stored->presets[2].key);
  EXPECT_EQ("", stored->active_preset);
}

TEST_F(Restore, TrimsAndClampsAsACreateDoes) {
  ClimateConfig doc = backup();
  doc.name = "  Den  ";
  doc.setpoint = 99.f;
  doc.presets[0].name = " Comfort ";
  doc.safety.sensor_timeout_s = 1e7f;
  ASSERT_TRUE(hub().restore(doc).ok);
  const ClimateConfig *stored = hub().store().get("lounge");
  EXPECT_EQ("Den", stored->name);
  EXPECT_EQ("Comfort", stored->presets[0].name);
  EXPECT_FLOAT_EQ(45.f, stored->setpoint);
  EXPECT_FLOAT_EQ(86400.f, stored->safety.sensor_timeout_s);
}

// In place: the same entity, the relay it holds handed on as it is, the file rewritten.
TEST_F(Restore, ReplacesTheThermostatThatHasTheIdInPlace) {
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  const RelayClaim *claim = hub().claim("relay_1");
  this->quiet();

  ClimateConfig doc = backup("boiler");
  doc.name = "Boiler";
  Result result = hub().restore(doc);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ("boiler", result.id);
  EXPECT_EQ(entity, hub().entity_of("boiler"));
  EXPECT_EQ(claim, hub().claim("relay_1")) << "a relay it holds is never refused, and stays held";
  EXPECT_EQ(1u, hub().store().size());
  const ClimateConfig *stored = hub().store().get("boiler");
  EXPECT_EQ("night-2", stored->active_preset) << "an update would have dropped a key it never gave out";
  EXPECT_EQ("day-time", this->on_flash("boiler").presets[0].key);
  pass_resync_delay();
  EXPECT_EQ(1, hub().resyncs) << "the preset list changed";
}

TEST_F(Restore, ADisabledDocumentStopsTheThermostatItReplaces) {
  this->create(draft("Boiler"));
  ClimateConfig doc = backup("boiler");
  doc.enabled = false;
  ASSERT_TRUE(hub().restore(doc).ok);
  EXPECT_FALSE(hub().is_running("boiler"));
  EXPECT_EQ("", hub().claimed_by("relay_1"));
  EXPECT_FALSE(this->on_flash("boiler").enabled);
}

TEST_F(Restore, AnEnabledDocumentStartsTheStoppedThermostatItReplaces) {
  ClimateConfig boiler = draft("Boiler");
  boiler.enabled = false;
  this->create(boiler);
  ASSERT_TRUE(hub().restore(backup("boiler")).ok);
  EXPECT_TRUE(hub().is_running("boiler"));
  EXPECT_EQ("Living Room", hub().store().get("boiler")->name);
}

// The id becomes the file's name, as the loader requires; `new` is the editor's blank form.
TEST_F(Restore, RefusesAnIdTheLoaderWouldRefuse) {
  struct Case {
    const char *id;
    const char *error;
  };
  for (const Case &c : {Case{"", "id is required"},
                        Case{"Living Room", "id must be a slug: lowercase letters, digits and single dashes"},
                        Case{"../boiler", "id must be a slug: lowercase letters, digits and single dashes"},
                        Case{"a--b", "id must be a slug: lowercase letters, digits and single dashes"},
                        Case{"new", "id 'new' is reserved"}}) {
    Result result = hub().restore(backup(c.id));
    EXPECT_EQ(400, result.code) << c.id;
    EXPECT_EQ(c.error, result.error) << c.id;
  }
  const std::string longest(ID_MAX_LENGTH, 'a');
  EXPECT_EQ(400, hub().restore(backup((longest + "a").c_str())).code);
  EXPECT_TRUE(list_dir(this->folder()).empty());
  EXPECT_TRUE(hub().restore(backup(longest.c_str(), "relay_2")).ok);
  ClimateConfig other = backup("new-2", "relay_3");
  other.name = "Other";
  EXPECT_TRUE(hub().restore(other).ok);
}

// The id first, as in a file; the reserved one only once the document is sound.
TEST_F(Restore, TheIdComesFirstAndItsReservedWordAfterTheDocument) {
  ClimateConfig doc = backup("");
  doc.sensor_id = "";
  EXPECT_EQ("id is required", hub().restore(doc).error);
  doc.id = "new";
  EXPECT_EQ("sensor_id is required", hub().restore(doc).error);
  doc = backup();
  doc.presets[1].key = "Night";
  EXPECT_EQ("Preset 2: key must be a slug: lowercase letters, digits and single dashes", hub().restore(doc).error);
  doc.presets[1].key = "day-time";
  EXPECT_EQ("Preset 2: key 'day-time' is already used by Preset 1", hub().restore(doc).error);
  EXPECT_TRUE(list_dir(this->folder()).empty());
}

// A file the boot refused holds its id, as it does against a create: it is the only record of
// what its author meant, and the restore names it rather than write over it.
TEST_F(Restore, RefusesAnIdAFileTheBootDidNotLoadHolds) {
  mkdir(this->folder().c_str(), 0755);
  write_file(this->file_of("lounge"), "left alone");
  this->reboot();
  ASSERT_EQ(0u, hub().store().size());

  Result result = hub().restore(backup());
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("The id \"lounge\" is taken by a file in the thermostat folder that was not loaded", result.error);
  EXPECT_EQ("left alone", read_file(this->file_of("lounge")));
  EXPECT_EQ(0u, hub().store().size());
}

TEST_F(Restore, ANewOneStopsAtTheLimitAndAReplacementDoesNot) {
  for (const char *relay : {"relay_1", "relay_2", "relay_3"}) {
    ClimateConfig config = draft(std::string("Room ") + relay, relay);
    this->create(config);
  }
  ClimateConfig spare = draft("Spare");
  spare.enabled = false;
  this->create(spare);

  ClimateConfig doc = backup();
  doc.enabled = false;
  Result refused = hub().restore(doc);
  EXPECT_EQ(507, refused.code);
  EXPECT_EQ("This device allows 4 thermostats; delete one to add another", refused.error);
  EXPECT_FALSE(file_exists(this->file_of("lounge")));

  doc.id = "spare";
  EXPECT_TRUE(hub().restore(doc).ok) << "the limit counts thermostats, and this one is there";
  EXPECT_EQ("Living Room", hub().store().get("spare")->name);
}

TEST_F(Restore, RefusesANameAnotherClimateAnswersToButNotItsOwn) {
  this->create(draft("Living Room", "relay_2"));
  Result taken = hub().restore(backup());
  EXPECT_EQ(409, taken.code);
  EXPECT_EQ("\"Living Room\" is already used by another thermostat", taken.error);

  ClimateConfig hall = backup();
  hall.name = "hall";
  EXPECT_EQ(409, hub().restore(hall).code) << "a YAML climate's name";

  ClimateConfig same = backup("living-room", "relay_2");
  EXPECT_TRUE(hub().restore(same).ok) << "its own name never blocks it";
  EXPECT_EQ(1u, hub().store().size());
}

TEST_F(Restore, AnEnabledOneIsRefusedARelayAnotherHoldsOrWaitsFor) {
  this->create(draft("Boiler"));
  Result held = hub().restore(backup());
  EXPECT_EQ(409, held.code);
  EXPECT_EQ("\"Relay 1\" is already driven by \"Boiler\"", held.error);
  EXPECT_EQ("boiler", held.holder);

  ClimateConfig attic = draft("Attic", "relay_2");
  attic.sensor_id = "gone";
  this->create(attic);
  Result reserved = hub().restore(backup("lounge", "relay_2"));
  EXPECT_EQ(409, reserved.code);
  EXPECT_EQ("\"Relay 2\" is reserved by \"Attic\", which is enabled and waits to start", reserved.error);
  EXPECT_EQ("attic", reserved.holder);

  ClimateConfig uptime = backup("lounge", "relay_3");
  uptime.sensor_id = "uptime";
  Result unit = hub().restore(uptime);
  EXPECT_EQ(400, unit.code);
  EXPECT_EQ("\"Uptime\" reports s, not °C", unit.error);

  ClimateConfig disabled = backup();
  disabled.enabled = false;
  EXPECT_TRUE(hub().restore(disabled).ok) << "a disabled one takes nothing";
}

TEST_F(Restore, AnEnabledOneWhoseSensorIsMissingIsStoredAndWaits) {
  ClimateConfig doc = backup();
  doc.sensor_id = "gone";
  Result created = hub().restore(doc);
  ASSERT_TRUE(created.ok) << created.error;
  EXPECT_EQ("not started: sensor 'gone' not found", created.warning);
  EXPECT_EQ("not started: sensor 'gone' not found", hub().waiting_reason("lounge"));
  EXPECT_TRUE(file_exists(this->file_of("lounge")));

  // Over a running one, it stops it and lets its relay go.
  this->create(draft("Boiler", "relay_2"));
  ClimateConfig moved = backup("boiler", "relay_2");
  moved.name = "Boiler";
  moved.sensor_id = "gone";
  Result replaced = hub().restore(moved);
  ASSERT_TRUE(replaced.ok) << replaced.error;
  EXPECT_EQ("not started: sensor 'gone' not found", replaced.warning);
  EXPECT_FALSE(hub().is_running("boiler"));
  EXPECT_EQ("", hub().claimed_by("relay_2"));
}

// The relay a replacement lets go starts the one that waited for it.
TEST_F(Restore, AReplacementThatFreesARelayStartsWhoWaitedForIt) {
  mkdir(this->folder().c_str(), 0755);
  write_file(this->file_of("summer"), file_doc("summer", "Summer", "relay_1"));
  write_file(this->file_of("winter"), file_doc("winter", "Winter", "relay_1"));
  this->reboot();
  ASSERT_TRUE(hub().is_running("summer"));
  ASSERT_FALSE(hub().is_running("winter"));

  Result result = hub().restore(backup("summer", "relay_2"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(std::vector<std::string>{"winter"}, result.started);
  EXPECT_EQ("winter", hub().claimed_by("relay_1"));
  EXPECT_EQ("summer", hub().claimed_by("relay_2"));
}

// What a newer firmware wrote is not this one's to rewrite, as for a Save.
TEST_F(Restore, RefusesToReplaceAThermostatANewerFirmwareWrote) {
  mkdir(this->folder().c_str(), 0755);
  const std::string text = R"({"version":3,"id":"boiler","name":"Boiler","sensor_id":"room",)"
                           R"("heat":{"relay_id":"relay_1"},"mode":"heat","future":1})";
  write_file(this->file_of("boiler"), text);
  this->reboot();
  ASSERT_TRUE(hub().store().get("boiler")->from_newer_firmware());

  Result result = hub().restore(backup("boiler"));
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("A newer firmware wrote this thermostat; update the firmware to change it", result.error);
  EXPECT_EQ(text, read_file(this->file_of("boiler")));
  EXPECT_EQ("Boiler", hub().store().get("boiler")->name);
}

// A backup from a newer firmware comes back in this firmware's format, as a Save writes it.
TEST_F(Restore, WritesItsOwnVersionWhateverTheDocumentSays) {
  ClimateConfig doc = backup();
  doc.version = 3;
  ASSERT_TRUE(hub().restore(doc).ok);
  EXPECT_FALSE(hub().store().get("lounge")->from_newer_firmware());
  EXPECT_EQ(CONFIG_VERSION, this->on_flash("lounge").version);
}

TEST_F(Restore, AFileThatCannotBeWrittenChangesNothing) {
  this->create(draft("Boiler"));
  const std::string before = read_file(this->file_of("boiler"));
  storage().path = "/proc/definitely-not-writable";
  Result created = hub().restore(backup("lounge", "relay_2"));
  Result replaced = hub().restore(backup("boiler"));
  storage().path = this->base_;

  for (const Result &result : {created, replaced}) {
    EXPECT_EQ(500, result.code);
    EXPECT_EQ("The thermostat's file could not be written", result.error);
    EXPECT_FALSE(result.persisted);
  }
  EXPECT_EQ(nullptr, hub().store().get("lounge"));
  EXPECT_EQ("Boiler", hub().store().get("boiler")->name);
  EXPECT_EQ(before, read_file(this->file_of("boiler")));
  EXPECT_TRUE(hub().is_running("boiler"));
}

TEST_F(Restore, AFileOverTheCapIsNeverWritten) {
  hub().max_file_bytes = 100;
  Result result = hub().restore(backup());
  EXPECT_EQ(413, result.code);
  EXPECT_EQ("The thermostat's file would be over 8 KiB", result.error);
  EXPECT_TRUE(list_dir(this->folder()).empty());
}

TEST_F(Restore, AHubWithoutStorageRefusesIt) {
  hub().mark_failed();
  Result result = hub().restore(backup());
  hub().reset_to_construction_state();
  EXPECT_EQ(500, result.code);
  EXPECT_EQ("Thermostat storage is not available", result.error);
  EXPECT_TRUE(list_dir(this->folder()).empty());
}

}  // namespace esphome::climate_hub::testing
