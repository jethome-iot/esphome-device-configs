// Turning a thermostat off and back on, as an automation rule does: mode off, then the mode it
// was in before, which its file keeps across a reboot. And what the hub tells a rule engine.
#include "common.h"

namespace esphome::climate_hub::testing {
namespace {

// Heats on relay_1, cools on relay_2, in mode cool.
ClimateConfig both(const char *name = "Boiler") {
  ClimateConfig c = draft(name);
  c.cool.relay_id = "relay_2";
  c.mode = HubMode::COOL;
  return c;
}

PresetConfig preset(const char *name, float setpoint, optional<HubMode> mode = nullopt) {
  PresetConfig p;
  p.name = name;
  p.setpoint = setpoint;
  p.mode = mode;
  return p;
}

// Every id the hub has reported since the last clear(). Registered once: the hub keeps its
// callbacks for the life of the process.
std::vector<std::string> &heard() {
  static std::vector<std::string> *ids = [] {
    auto *v = new std::vector<std::string>();
    hub().add_on_change_callback([v](const std::string &id) { v->push_back(id); });
    return v;
  }();
  return *ids;
}

class OnOff : public HubTest {
 protected:
  // Lets the debounce pass and writes what is dirty.
  static void flush() {
    hub().ms += 3000;
    hub().loop();
  }

  static void mode(HubClimate *entity, climate::ClimateMode mode) {
    auto call = entity->make_call();
    call.set_mode(mode);
    call.perform();
  }
};

}  // namespace

TEST(OnMode, IsTheModeWhileOnElseTheLastOneElseHeatOrCool) {
  ClimateConfig heat = draft("Heat");
  EXPECT_EQ(HubMode::HEAT, heat.on_mode());
  heat.set_mode(HubMode::OFF);
  EXPECT_EQ(HubMode::HEAT, heat.last_on_mode);
  EXPECT_EQ(HubMode::HEAT, heat.on_mode());

  ClimateConfig two = both();
  two.set_mode(HubMode::HEAT_COOL);
  two.set_mode(HubMode::OFF);
  EXPECT_EQ(HubMode::HEAT_COOL, two.on_mode());
  two.set_mode(HubMode::OFF);
  EXPECT_EQ(HubMode::HEAT_COOL, two.on_mode()) << "off twice is still the mode before off";
  two.cool.relay_id = "";
  EXPECT_EQ(HubMode::HEAT, two.on_mode()) << "a mode the relays no longer serve gives way";

  ClimateConfig cool = draft("Cool", "");
  cool.cool.relay_id = "relay_2";
  cool.mode = HubMode::OFF;
  EXPECT_EQ(HubMode::COOL, cool.on_mode()) << "cooling only, never off before";
}

// A file from before the field: the mode it is in, or with mode off the default.
TEST(OnMode, AFileWithoutOneReadsItsMode) {
  ClimateConfig parsed;
  std::string error;
  ASSERT_TRUE(from_json(R"({"version":2,"name":"B","sensor_id":"s","heat":{"relay_id":"r"},)"
                        R"("cool":{"relay_id":"c"},"mode":"cool"})",
                        &parsed, &error, false))
      << error;
  EXPECT_EQ(HubMode::COOL, parsed.on_mode());
  EXPECT_NE(std::string::npos, to_json(parsed).find(R"("mode":"cool","last_on_mode":"cool",)"));

  ASSERT_TRUE(from_json(R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"},"cool":{"relay_id":"c"},)"
                        R"("mode":"off"})",
                        &parsed, &error, false))
      << error;
  EXPECT_EQ(HubMode::HEAT, parsed.on_mode());
}

TEST(OnMode, AFileKeepsTheModeBeforeOff) {
  ClimateConfig parsed;
  std::string error;
  ASSERT_TRUE(from_json(R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"},"cool":{"relay_id":"c"},)"
                        R"("mode":"off","last_on_mode":"heat_cool"})",
                        &parsed, &error, false))
      << error;
  EXPECT_EQ(HubMode::HEAT_COOL, parsed.on_mode());
  EXPECT_NE(std::string::npos, to_json(parsed).find(R"("mode":"off","last_on_mode":"heat_cool",)"));
}

// State, not a rule: one the relays do not serve is no reason to refuse the thermostat.
TEST(OnMode, OneTheRelaysDoNotServeGivesWay) {
  ClimateConfig parsed;
  std::string error;
  ASSERT_TRUE(from_json(R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"},"mode":"off",)"
                        R"("last_on_mode":"cool"})",
                        &parsed, &error, false))
      << error;
  EXPECT_EQ(HubMode::HEAT, parsed.on_mode());
  EXPECT_NE(std::string::npos, to_json(parsed).find(R"("last_on_mode":"heat")"));
}

TEST(OnMode, AWordItDoesNotKnowIsRefused) {
  for (const char *word : {R"("dry")", R"("off")", R"("")", "2"}) {
    ClimateConfig parsed;
    std::string error;
    EXPECT_FALSE(from_json(std::string(R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"},"mode":"heat",)") +
                               R"("last_on_mode":)" + word + "}",
                           &parsed, &error, false))
        << word;
    EXPECT_EQ("last_on_mode must be one of heat/cool/heat_cool", error) << word;
  }
}

// A preset that turns the thermostat off keeps the mode it left, as a mode set by hand does.
TEST(OnMode, APresetWithModeOffKeepsTheModeItLeft) {
  ClimateConfig c = both();
  c.pick_preset(preset("Away", 12.f, HubMode::OFF));
  EXPECT_EQ(HubMode::OFF, c.mode);
  EXPECT_EQ(HubMode::COOL, c.on_mode());
}

TEST_F(OnOff, TurnsARunningThermostatOffAndBackToItsMode) {
  this->create(both());
  HubClimate *entity = hub().entity_of("boiler");
  ASSERT_NE(nullptr, entity);

  Result result = hub().turn_off("boiler");
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.persisted);
  EXPECT_EQ(climate::CLIMATE_MODE_OFF, entity->mode);
  EXPECT_EQ(HubMode::OFF, hub().store().get("boiler")->mode);
  EXPECT_TRUE(hub().dirty("boiler"));
  flush();
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find(R"("mode":"off","last_on_mode":"cool",)"));

  result = hub().turn_on("boiler");
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(climate::CLIMATE_MODE_COOL, entity->mode);
  EXPECT_EQ(HubMode::COOL, hub().store().get("boiler")->mode);
}

// Home Assistant's mode off is a turn-off too: the mode before it comes back after a reboot.
TEST_F(OnOff, TheModeBeforeOffSurvivesAReboot) {
  this->create(both());
  mode(hub().entity_of("boiler"), climate::CLIMATE_MODE_HEAT_COOL);
  mode(hub().entity_of("boiler"), climate::CLIMATE_MODE_OFF);
  flush();
  this->reboot();
  ASSERT_EQ(HubMode::OFF, hub().store().get("boiler")->mode);

  ASSERT_TRUE(hub().turn_on("boiler").ok);
  EXPECT_EQ(climate::CLIMATE_MODE_HEAT_COOL, hub().entity_of("boiler")->mode);
}

// Running or not, as a target is: a stopped thermostat starts in the mode it was given.
TEST_F(OnOff, AStoppedThermostatStartsInTheModeItWasGiven) {
  this->create(both());
  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  flush();

  ASSERT_TRUE(hub().turn_off("boiler").ok);
  EXPECT_EQ(HubMode::OFF, hub().store().get("boiler")->mode);
  EXPECT_TRUE(hub().dirty("boiler"));
  ASSERT_TRUE(hub().set_enabled("boiler", true).ok);
  EXPECT_EQ(climate::CLIMATE_MODE_OFF, hub().entity_of("boiler")->mode);

  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  flush();
  ASSERT_TRUE(hub().turn_on("boiler").ok);
  EXPECT_EQ(HubMode::COOL, hub().store().get("boiler")->mode);
  EXPECT_TRUE(hub().dirty("boiler"));
}

TEST_F(OnOff, TurningOnWhatIsOnChangesNothing) {
  this->create(both());
  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  flush();
  ASSERT_TRUE(hub().turn_on("boiler").ok);
  EXPECT_FALSE(hub().dirty("boiler"));

  ASSERT_TRUE(hub().set_enabled("boiler", true).ok);
  ASSERT_TRUE(hub().turn_on("boiler").ok);
  EXPECT_FALSE(hub().dirty("boiler"));
}

TEST_F(OnOff, AnUnknownThermostatIsNotFound) {
  for (const Result &result : {hub().turn_on("ghost"), hub().turn_off("ghost")}) {
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(404, result.code);
    EXPECT_EQ("Thermostat not found", result.error);
  }
}

// The mode to come back to is the thermostat's, not the form's.
TEST_F(OnOff, ASaveAndACreateIgnoreTheDocumentsOwn) {
  ClimateConfig config = both();
  config.last_on_mode = HubMode::HEAT_COOL;
  this->create(config);
  ASSERT_TRUE(hub().turn_off("boiler").ok);
  EXPECT_EQ(HubMode::COOL, hub().store().get("boiler")->on_mode()) << "the create started from its mode";

  ClimateConfig doc = *hub().store().get("boiler");
  doc.last_on_mode = HubMode::HEAT;
  doc.setpoint = 23.f;
  ASSERT_TRUE(hub().update("boiler", doc).ok);
  EXPECT_EQ(HubMode::COOL, hub().store().get("boiler")->on_mode());

  // A Save that turns it off keeps the mode it was in.
  ASSERT_TRUE(hub().turn_on("boiler").ok);
  doc = *hub().store().get("boiler");
  doc.mode = HubMode::OFF;
  ASSERT_TRUE(hub().update("boiler", doc).ok);
  EXPECT_EQ(HubMode::COOL, hub().store().get("boiler")->on_mode());
}

// A newer firmware's file is never written: the change lasts until the next boot.
TEST_F(OnOff, ANewerFirmwaresThermostatTurnsOffInMemory) {
  const std::string text = R"({"version":)" + std::to_string(CONFIG_VERSION + 1) +
                           R"(,"id":"boiler","name":"Boiler","kind":"bang_bang","sensor_id":"room",)"
                           R"("heat":{"relay_id":"relay_1"},"mode":"heat","setpoint":21})";
  write_file(this->file_of("boiler"), text);
  this->reboot();
  Result result = hub().turn_off("boiler");
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.persisted);
  EXPECT_EQ(HubMode::OFF, hub().store().get("boiler")->mode);
  flush();
  EXPECT_EQ(text, read_file(this->file_of("boiler")));
}

TEST_F(OnOff, TheHubReportsACreateARemovalAndNewPresetKeys) {
  heard().clear();
  this->create(both());
  EXPECT_EQ((std::vector<std::string>{"boiler"}), heard());

  heard().clear();
  EXPECT_FALSE(hub().create(both()).ok) << "the name is taken";
  ClimateConfig doc = *hub().store().get("boiler");
  doc.name = "Kettle";
  ASSERT_TRUE(hub().update("boiler", doc).ok);
  ASSERT_TRUE(hub().turn_off("boiler").ok);
  EXPECT_TRUE(heard().empty()) << "nothing a rule names changed";

  doc = *hub().store().get("boiler");
  doc.presets.push_back(preset("Eco", 18.f));
  ASSERT_TRUE(hub().update("boiler", doc).ok);
  EXPECT_EQ((std::vector<std::string>{"boiler"}), heard());

  heard().clear();
  doc = *hub().store().get("boiler");
  doc.presets[0].setpoint = 17.f;
  doc.presets[0].name = "Economy";
  ASSERT_TRUE(hub().update("boiler", doc).ok);
  EXPECT_TRUE(heard().empty()) << "the key stays across a rename";

  ASSERT_FALSE(hub().remove("ghost").ok);
  ASSERT_TRUE(hub().remove("boiler").ok);
  EXPECT_EQ((std::vector<std::string>{"boiler"}), heard());
}

// A restore brings back the mode the backup goes back on in, as it brings back its active preset:
// the backup's, not the replaced thermostat's, which a Save keeps.
TEST_F(OnOff, ARestoreTakesTheBackupsModeToGoBackTo) {
  this->create(both());
  ASSERT_TRUE(hub().turn_off("boiler").ok);
  ClimateConfig doc = *hub().store().get("boiler");
  ASSERT_EQ(HubMode::COOL, doc.on_mode());

  doc.last_on_mode = HubMode::HEAT_COOL;
  ASSERT_TRUE(hub().restore(doc).ok);
  EXPECT_EQ(HubMode::HEAT_COOL, hub().store().get("boiler")->on_mode());
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find(R"("mode":"off","last_on_mode":"heat_cool",)"));
  ASSERT_TRUE(hub().turn_on("boiler").ok);
  EXPECT_EQ(climate::CLIMATE_MODE_HEAT_COOL, hub().entity_of("boiler")->mode);

  // A backup from before the field reads as a file without one: heat, whatever the thermostat had.
  doc.last_on_mode = HubMode::OFF;
  ASSERT_TRUE(hub().restore(doc).ok);
  EXPECT_EQ(HubMode::HEAT, hub().store().get("boiler")->on_mode());
}

// To the rules, a restore that adds a thermostat is a create, and one that replaces it a Save.
TEST_F(OnOff, TheHubReportsARestoreThatAddsOneOrChangesItsPresetKeys) {
  heard().clear();
  ClimateConfig doc = both();
  doc.id = "kettle";
  ASSERT_TRUE(hub().restore(doc).ok);
  EXPECT_EQ((std::vector<std::string>{"kettle"}), heard());

  heard().clear();
  doc.setpoint = 23.f;
  ASSERT_TRUE(hub().restore(doc).ok);
  EXPECT_TRUE(heard().empty()) << "the same preset keys";

  doc.presets.push_back(preset("Eco", 18.f));
  ASSERT_TRUE(hub().restore(doc).ok);
  EXPECT_EQ((std::vector<std::string>{"kettle"}), heard());
}

}  // namespace esphome::climate_hub::testing
