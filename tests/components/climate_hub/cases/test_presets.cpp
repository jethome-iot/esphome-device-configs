// Presets on the hub: what Home Assistant lists and picks, what a Save does to them, the label
// across a reboot, and a file from a newer firmware that runs but is never written.
#include "common.h"

namespace esphome::climate_hub::testing {
namespace {

PresetConfig preset(const char *name, float setpoint, optional<HubMode> mode = nullopt, const char *key = "") {
  PresetConfig p;
  p.key = key;
  p.name = name;
  p.setpoint = setpoint;
  p.mode = mode;
  return p;
}

// Heats on relay_1 and cools on relay_2, a bang-bang at 21 with two built-in presets and two
// custom ones, in that order.
ClimateConfig with_presets(const char *name = "Boiler") {
  ClimateConfig c = draft(name);
  c.kind = ControlKind::BANG_BANG;
  c.cool.relay_id = "relay_2";
  c.setpoint = 21.f;
  c.presets = {preset("Eco", 18.f), preset("away", 12.f, HubMode::OFF), preset("Night", 19.5f, HubMode::HEAT_COOL),
               preset("Day", 22.f)};
  return c;
}

// The stored thermostat as the editor gets it back, presets keyed.
ClimateConfig stored(const std::string &id) { return *hub().store().get(id); }

class Presets : public HubTest {
 protected:
  static void pick(HubClimate *entity, climate::ClimatePreset preset) {
    auto call = entity->make_call();
    call.set_preset(preset);
    call.perform();
  }

  static void pick(HubClimate *entity, const char *name) {
    auto call = entity->make_call();
    call.set_preset(name);
    call.perform();
  }

  static void target(HubClimate *entity, float value) {
    auto call = entity->make_call();
    call.set_target_temperature(value);
    call.perform();
  }

  static void mode(HubClimate *entity, climate::ClimateMode mode) {
    auto call = entity->make_call();
    call.set_mode(mode);
    call.perform();
  }

  // What the entity says is active: the built-in preset's name, the custom one's, or "".
  static std::string label(HubClimate *entity) {
    if (entity->has_custom_preset())
      return entity->get_custom_preset().c_str();
    if (entity->preset.has_value())
      return LOG_STR_ARG(climate::climate_preset_to_string(*entity->preset));
    return "";
  }

  static std::vector<std::string> custom_presets(HubClimate *entity) {
    std::vector<std::string> names;
    for (const char *name : entity->get_traits().get_supported_custom_presets())
      names.emplace_back(name);
    return names;
  }

  // Lets the debounce pass and writes what is dirty.
  static void flush() {
    hub().ms += 3000;
    hub().loop();
  }

  void quiet() {
    pass_resync_delay();
    hub().resyncs = 0;
  }
};

}  // namespace

// The keys are the hub's to give, as the id is: made from the names, never from the draft.
TEST_F(Presets, CreateGivesEveryPresetAKeyFromItsName) {
  ClimateConfig config = with_presets();
  config.presets[0].key = "chosen";
  config.presets.push_back(preset("  night! ", 17.f));
  config.active_preset = "chosen";
  this->create(config);

  const ClimateConfig *saved = hub().store().get("boiler");
  std::vector<std::string> keys;
  for (const PresetConfig &p : saved->presets)
    keys.push_back(p.key);
  EXPECT_EQ((std::vector<std::string>{"eco", "away", "night", "day", "night-2"}), keys);
  EXPECT_EQ("night!", saved->presets[4].name) << "trimmed";
  EXPECT_EQ("", saved->active_preset) << "a new thermostat has no preset picked";
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find(R"("key":"night-2")"));
}

TEST_F(Presets, ACreateRefusesABrokenPreset) {
  ClimateConfig config = with_presets();
  config.presets[3].name = "NONE";
  Result result = hub().create(config);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("Preset 4: \"NONE\" is reserved", result.error);
  EXPECT_TRUE(list_dir(this->folder()).empty());
}

// A built-in name goes into the preset mask, a custom one into the custom list, in order.
TEST_F(Presets, TheEntityListsBuiltInAndCustomPresets) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  auto traits = entity->get_traits();
  EXPECT_TRUE(traits.supports_preset(climate::CLIMATE_PRESET_ECO));
  EXPECT_TRUE(traits.supports_preset(climate::CLIMATE_PRESET_AWAY));
  EXPECT_FALSE(traits.supports_preset(climate::CLIMATE_PRESET_BOOST));
  EXPECT_FALSE(traits.supports_preset(climate::CLIMATE_PRESET_NONE));
  EXPECT_EQ((std::vector<std::string>{"Night", "Day"}), custom_presets(entity));
  EXPECT_EQ("", label(entity));

  ClimateConfig none = draft("Kettle", "relay_3");
  this->create(none);
  traits = hub().entity_of("kettle")->get_traits();
  EXPECT_FALSE(traits.get_supports_presets());
  EXPECT_TRUE(traits.get_supported_custom_presets().empty());
}

// The web server walks the custom list on its own task: a Save rewrites the names in the
// slot's own buffers and never moves the list, nor does a park or the eight names after it.
TEST_F(Presets, TheCustomNamesStayInTheSlotsBuffers) {
  ClimateConfig config = with_presets();
  config.presets[3].name = std::string(NAME_MAX_LENGTH, 'd');
  this->create(config);
  HubClimate *entity = hub().entity_of("boiler");
  const std::vector<const char *> &list = entity->get_traits().get_supported_custom_presets();
  const char *const *storage = list.data();
  const char *first = list[0];
  EXPECT_EQ(NAME_MAX_LENGTH, strlen(list[1]));

  // The built-in two make way for six more: eight custom names, the most there can be.
  config = stored("boiler");
  config.presets[2].name = "Late night";
  config.presets.erase(config.presets.begin(), config.presets.begin() + 2);
  for (int i = 0; i < 6; i++)
    config.presets.push_back(preset(("Extra " + std::to_string(i)).c_str(), 20.f));
  ASSERT_TRUE(hub().update("boiler", config).ok);
  const std::vector<const char *> &after = entity->get_traits().get_supported_custom_presets();
  EXPECT_EQ(&list, &after);
  ASSERT_EQ(PRESET_MAX_COUNT, after.size());
  EXPECT_EQ(storage, after.data()) << "eight names fit the room the slot set aside";
  EXPECT_EQ(first, after[0]);
  EXPECT_STREQ("Late night", after[0]);
  EXPECT_STREQ("Extra 5", after[7]);

  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  entity->park(0);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(storage, list.data()) << "parked";
  entity->set_presets(stored("boiler").presets);
  ASSERT_EQ(PRESET_MAX_COUNT, list.size());
  EXPECT_EQ(storage, list.data()) << "eight names again";
  EXPECT_EQ(first, list[0]);
}

// The slot has room for eight custom names, however many it is handed.
TEST_F(Presets, ASlotListsAtMostEightCustomNames) {
  HubClimate *entity = hub().slot_entity(0);
  std::vector<PresetConfig> many;
  for (int i = 0; i < 10; i++)
    many.push_back(preset(("Custom " + std::to_string(i)).c_str(), 20.f));
  many.push_back(preset("Eco", 18.f));
  entity->set_presets(many);
  const std::vector<std::string> names = custom_presets(entity);
  ASSERT_EQ(PRESET_MAX_COUNT, names.size());
  EXPECT_EQ("Custom 7", names.back());
  EXPECT_TRUE(entity->get_traits().supports_preset(climate::CLIMATE_PRESET_ECO)) << "only the custom names are capped";
  entity->park(0);
}

// A built-in preset from Home Assistant: its target, and the thermostat's own mode for "keep".
TEST_F(Presets, HomeAssistantPicksABuiltInPreset) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  pick(entity, climate::CLIMATE_PRESET_ECO);

  const ClimateConfig *saved = hub().store().get("boiler");
  EXPECT_FLOAT_EQ(18.f, saved->setpoint);
  EXPECT_EQ(HubMode::HEAT, saved->mode);
  EXPECT_EQ("eco", saved->active_preset);
  EXPECT_FLOAT_EQ(18.f, entity->target_temperature);
  EXPECT_EQ("ECO", label(entity));
  EXPECT_TRUE(hub().dirty("boiler"));
  flush();
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find(R"("active_preset":"eco")"));

  // One with a mode sets it.
  pick(entity, climate::CLIMATE_PRESET_AWAY);
  EXPECT_EQ(HubMode::OFF, saved->mode);
  EXPECT_EQ(climate::CLIMATE_MODE_OFF, entity->mode);
  EXPECT_EQ("AWAY", label(entity));
}

// Home Assistant sends a custom preset by its name; upstream maps a standard name in any case
// to the built-in one first, and so does the hub's lookup.
TEST_F(Presets, HomeAssistantPicksACustomPresetByName) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  pick(entity, "Night");

  const ClimateConfig *saved = hub().store().get("boiler");
  EXPECT_FLOAT_EQ(19.5f, saved->setpoint);
  EXPECT_EQ(HubMode::HEAT_COOL, saved->mode);
  EXPECT_EQ("night", saved->active_preset);
  EXPECT_EQ(climate::CLIMATE_MODE_HEAT_COOL, entity->mode);
  EXPECT_EQ("Night", label(entity));
  EXPECT_FALSE(entity->preset.has_value());

  pick(entity, "eco");
  EXPECT_EQ("eco", saved->active_preset);
  EXPECT_EQ("ECO", label(entity));
  EXPECT_FALSE(entity->has_custom_preset());
}

// Picking what is already picked, with nothing moved since, is no change to write.
TEST_F(Presets, PickingThePresetInForceChangesNothing) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  pick(entity, "Day");
  flush();
  ASSERT_FALSE(hub().dirty("boiler"));
  pick(entity, "Day");
  EXPECT_FALSE(hub().dirty("boiler"));
}

// A preset the thermostat does not list never reaches it: upstream drops it on the way in.
TEST_F(Presets, APresetTheThermostatDoesNotHaveIsIgnored) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  pick(entity, climate::CLIMATE_PRESET_BOOST);
  pick(entity, "Morning");
  EXPECT_FLOAT_EQ(21.f, hub().store().get("boiler")->setpoint);
  EXPECT_EQ("", hub().store().get("boiler")->active_preset);
  EXPECT_FALSE(hub().dirty("boiler"));
}

// A target or a mode set by hand keeps the label, as upstream's thermostat does.
TEST_F(Presets, AManualChangeKeepsTheLabel) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  pick(entity, "Night");
  target(entity, 23.f);
  mode(entity, climate::CLIMATE_MODE_HEAT);

  const ClimateConfig *saved = hub().store().get("boiler");
  EXPECT_FLOAT_EQ(23.f, saved->setpoint);
  EXPECT_EQ(HubMode::HEAT, saved->mode);
  EXPECT_EQ("night", saved->active_preset);
  EXPECT_EQ("Night", label(entity));

  ASSERT_TRUE(hub().set_setpoint("boiler", 24.f).ok);
  EXPECT_EQ("Night", label(entity));
}

// A call with a preset and a target takes the preset first and the target over it.
TEST_F(Presets, ATargetBesideAPresetWins) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  auto call = entity->make_call();
  call.set_preset("Night");
  call.set_target_temperature(25.f);
  call.set_mode(climate::CLIMATE_MODE_COOL);
  call.perform();

  const ClimateConfig *saved = hub().store().get("boiler");
  EXPECT_FLOAT_EQ(25.f, saved->setpoint);
  EXPECT_EQ(HubMode::COOL, saved->mode);
  EXPECT_EQ("night", saved->active_preset);
}

// The label comes back with the thermostat; its values are not applied again, so a target set
// by hand after the pick survives the reboot.
TEST_F(Presets, TheActivePresetIsRestoredAtBoot) {
  this->create(with_presets());
  pick(hub().entity_of("boiler"), "Night");
  target(hub().entity_of("boiler"), 23.f);
  this->reboot();

  HubClimate *entity = hub().entity_of("boiler");
  ASSERT_NE(nullptr, entity);
  EXPECT_EQ("Night", label(entity));
  EXPECT_FLOAT_EQ(23.f, entity->target_temperature);
  EXPECT_EQ(climate::CLIMATE_MODE_HEAT_COOL, entity->mode);
  EXPECT_EQ((std::vector<std::string>{"Night", "Day"}), custom_presets(entity));

  pick(entity, climate::CLIMATE_PRESET_AWAY);
  this->reboot();
  EXPECT_EQ("AWAY", label(hub().entity_of("boiler")));
  EXPECT_EQ("away", hub().store().get("boiler")->active_preset);
}

// New values for the active preset are what the thermostat runs at once; a values-only Save
// costs no reconnect.
TEST_F(Presets, EditingTheActivePresetAppliesItAtOnce) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  pick(entity, climate::CLIMATE_PRESET_ECO);
  this->quiet();

  ClimateConfig config = stored("boiler");
  config.presets[0].setpoint = 17.f;
  ASSERT_TRUE(hub().update("boiler", config).ok);
  EXPECT_FLOAT_EQ(17.f, hub().store().get("boiler")->setpoint);
  EXPECT_FLOAT_EQ(17.f, entity->target_temperature);
  EXPECT_EQ("ECO", label(entity));

  config = stored("boiler");
  config.presets[0].mode = HubMode::COOL;
  ASSERT_TRUE(hub().update("boiler", config).ok);
  EXPECT_EQ(HubMode::COOL, hub().store().get("boiler")->mode);
  EXPECT_EQ(climate::CLIMATE_MODE_COOL, entity->mode);

  // Another preset's values, or the form's own target, leave the running one alone.
  config = stored("boiler");
  config.presets[2].setpoint = 30.f;
  config.setpoint = 20.f;
  ASSERT_TRUE(hub().update("boiler", config).ok);
  EXPECT_FLOAT_EQ(20.f, entity->target_temperature);
  EXPECT_EQ("eco", hub().store().get("boiler")->active_preset);

  // A narrower range clamps the active preset's target, which is a new value too.
  config = stored("boiler");
  config.visual.min_temperature = 19.f;
  ASSERT_TRUE(hub().update("boiler", config).ok);
  EXPECT_FLOAT_EQ(19.f, hub().store().get("boiler")->presets[0].setpoint);
  EXPECT_FLOAT_EQ(19.f, entity->target_temperature);

  pass_resync_delay();
  EXPECT_EQ(1, hub().resyncs) << "only the new range is news to Home Assistant";
}

// A preset keeps its key through a rename; one the thermostat never gave out is made again from
// the name; the active preset is the thermostat's, not the form's.
TEST_F(Presets, ASaveKeepsTheKeysItGaveOut) {
  this->create(with_presets());
  pick(hub().entity_of("boiler"), climate::CLIMATE_PRESET_ECO);

  ClimateConfig config = stored("boiler");
  config.presets[2].name = "Late";
  config.presets.push_back(preset("Night", 16.f));
  config.presets.push_back(preset("Morning", 20.f, nullopt, "invented"));
  config.active_preset = "night";
  Result result = hub().update("boiler", config);
  ASSERT_TRUE(result.ok) << result.error;

  const ClimateConfig *saved = hub().store().get("boiler");
  ASSERT_EQ(6u, saved->presets.size());
  EXPECT_EQ("night", saved->presets[2].key);
  EXPECT_EQ("Late", saved->presets[2].name);
  EXPECT_EQ("night-2", saved->presets[4].key);
  EXPECT_EQ("morning", saved->presets[5].key);
  EXPECT_EQ("eco", saved->active_preset);
}

// The label goes with the preset it named.
TEST_F(Presets, RemovingTheActivePresetDropsTheLabel) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  pick(entity, "Night");

  ClimateConfig config = stored("boiler");
  config.presets.erase(config.presets.begin() + 2);
  ASSERT_TRUE(hub().update("boiler", config).ok);
  EXPECT_EQ("", hub().store().get("boiler")->active_preset);
  EXPECT_EQ("", label(entity));
  EXPECT_FLOAT_EQ(19.5f, entity->target_temperature) << "the values it left stay";
  EXPECT_EQ((std::vector<std::string>{"Day"}), custom_presets(entity));
}

// Home Assistant reads the preset list only when it lists the entities: the built-in presets as a
// set, the custom names in order. A running thermostat whose list changes makes it reconnect;
// new values do not, nor does any change to a thermostat that is not running.
TEST_F(Presets, HomeAssistantReconnectsWhenThePresetListChanges) {
  this->create(with_presets());
  this->quiet();

  const std::pair<const char *, void (*)(ClimateConfig &)> quiet_changes[] = {
      {"a new target", [](ClimateConfig &c) { c.presets[3].setpoint = 23.f; }},
      {"a new mode", [](ClimateConfig &c) { c.presets[3].mode = HubMode::COOL; }},
      {"another case of a built-in name", [](ClimateConfig &c) { c.presets[0].name = "ECO"; }},
      {"built-in presets in another order", [](ClimateConfig &c) { std::swap(c.presets[0], c.presets[1]); }},
  };
  for (const auto &change : quiet_changes) {
    ClimateConfig config = stored("boiler");
    change.second(config);
    ASSERT_TRUE(hub().update("boiler", config).ok) << change.first;
    pass_resync_delay();
    EXPECT_EQ(0, hub().resyncs) << change.first;
  }

  const std::pair<const char *, void (*)(ClimateConfig &)> listed_changes[] = {
      {"a new preset", [](ClimateConfig &c) { c.presets.push_back(preset("Party", 24.f)); }},
      {"a renamed one", [](ClimateConfig &c) { c.presets[2].name = "Late"; }},
      {"custom presets in another order", [](ClimateConfig &c) { std::swap(c.presets[2], c.presets[3]); }},
      {"a removed one", [](ClimateConfig &c) { c.presets.pop_back(); }},
      {"a custom one turned built-in", [](ClimateConfig &c) { c.presets[2].name = "Sleep"; }},
      {"a built-in one removed", [](ClimateConfig &c) { c.presets.erase(c.presets.begin()); }},
      {"a built-in one added", [](ClimateConfig &c) { c.presets.push_back(preset("Boost", 25.f)); }},
  };
  for (const auto &change : listed_changes) {
    ClimateConfig config = stored("boiler");
    change.second(config);
    ASSERT_TRUE(hub().update("boiler", config).ok) << change.first;
    const int before = hub().resyncs;
    pass_resync_delay();
    EXPECT_EQ(before + 1, hub().resyncs) << change.first;
  }

  ClimateConfig config = stored("boiler");
  config.enabled = false;
  ASSERT_TRUE(hub().update("boiler", config).ok);
  this->quiet();
  config = stored("boiler");
  config.presets.push_back(preset("Party", 24.f));
  ASSERT_TRUE(hub().update("boiler", config).ok);
  pass_resync_delay();
  EXPECT_EQ(0, hub().resyncs) << "a stopped thermostat is not listed";
}

// What a rule or the panel will call: a preset by its key, running or not.
TEST_F(Presets, ApplyPresetPicksByKey) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  ASSERT_TRUE(hub().apply_preset("boiler", "night").ok);
  EXPECT_FLOAT_EQ(19.5f, entity->target_temperature);
  EXPECT_EQ(climate::CLIMATE_MODE_HEAT_COOL, entity->mode);
  EXPECT_EQ("Night", label(entity));
  EXPECT_TRUE(hub().dirty("boiler"));
  flush();

  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  ASSERT_TRUE(hub().apply_preset("boiler", "away").ok);
  const ClimateConfig *saved = hub().store().get("boiler");
  EXPECT_FLOAT_EQ(12.f, saved->setpoint);
  EXPECT_EQ(HubMode::OFF, saved->mode);
  EXPECT_EQ("away", saved->active_preset);
  EXPECT_TRUE(hub().dirty("boiler"));
  flush();
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find(R"("active_preset":"away")"));
  ASSERT_TRUE(hub().apply_preset("boiler", "away").ok);
  EXPECT_FALSE(hub().dirty("boiler")) << "nothing moved";

  Result result = hub().apply_preset("nope", "eco");
  EXPECT_EQ(404, result.code);
  EXPECT_EQ("Thermostat not found", result.error);
  result = hub().apply_preset("boiler", "party");
  EXPECT_EQ(404, result.code);
  EXPECT_EQ("Preset not found", result.error);
  result = hub().apply_preset("boiler", "");
  EXPECT_EQ(404, result.code);
}

// A stopped thermostat's entity shows no preset; a parked slot lists none either.
TEST_F(Presets, AStoppedThermostatShowsNoPreset) {
  this->create(with_presets());
  HubClimate *entity = hub().entity_of("boiler");
  pick(entity, "Night");
  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  EXPECT_EQ("", label(entity));
  EXPECT_EQ((std::vector<std::string>{"Night", "Day"}), custom_presets(entity))
      << "hidden, it keeps its traits for a client still encoding it";

  entity->park(0);
  EXPECT_TRUE(custom_presets(entity).empty());
  EXPECT_FALSE(entity->get_traits().get_supports_presets());
}

// A file a newer firmware wrote runs as far as this one understands it, and is never written:
// changes stay in memory, and an editor Save is refused.
TEST_F(Presets, AFileFromANewerFirmwareRunsButIsNeverWritten) {
  const std::string text =
      R"({"version":3,"id":"boiler","name":"Boiler","kind":"bang_bang","sensor_id":"room",)"
      R"("heat":{"relay_id":"relay_1"},"mode":"heat","setpoint":21,"future":{"x":1},)"
      R"("presets":[{"key":"eco","name":"Eco","setpoint":18,"mode":"keep","colour":"green"}],"active_preset":"eco"})";
  write_file(this->file_of("boiler"), text);
  LogCapture::instance().clear();
  this->reboot();
  EXPECT_TRUE(LogCapture::instance().has("is version 3, from a newer firmware"));

  ASSERT_TRUE(hub().is_running("boiler"));
  const ClimateConfig *saved = hub().store().get("boiler");
  EXPECT_TRUE(saved->from_newer_firmware());
  HubClimate *entity = hub().entity_of("boiler");
  EXPECT_EQ("ECO", label(entity));

  target(entity, 23.f);
  EXPECT_FLOAT_EQ(23.f, saved->setpoint) << "in memory";
  EXPECT_FALSE(hub().dirty("boiler"));
  ASSERT_TRUE(hub().set_setpoint("boiler", 24.f).ok);
  ASSERT_TRUE(hub().apply_preset("boiler", "eco").ok);
  EXPECT_FLOAT_EQ(18.f, saved->setpoint);
  hub().on_shutdown();
  EXPECT_EQ(text, read_file(this->file_of("boiler")));

  Result result = hub().update("boiler", stored("boiler"));
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("A newer firmware wrote this thermostat; update the firmware to change it", result.error);

  result = hub().set_enabled("boiler", false);
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.persisted) << "stopped until the next boot";
  EXPECT_FALSE(hub().is_running("boiler"));
  EXPECT_EQ(text, read_file(this->file_of("boiler")));

  LogCapture::instance().clear();
  hub().dump_config();
  EXPECT_TRUE(LogCapture::instance().has("'Boiler' (boiler): bang_bang, disabled, file from a newer firmware"));

  // Removing is no rewrite: what the person asked for is gone.
  EXPECT_TRUE(hub().remove("boiler").ok);
  EXPECT_FALSE(file_exists(this->file_of("boiler")));
}

// A take-over by one whose file a newer firmware wrote cannot be recorded in that file, so the
// holder is stopped in memory only: the next boot runs what the files say, not neither of them.
TEST_F(Presets, ATakeOverByANewerFileLastsUntilTheReboot) {
  this->create(draft("Winter"));
  const std::string text = R"({"version":3,"id":"summer","name":"Summer","enabled":false,"sensor_id":"room",)"
                           R"("heat":{"relay_id":"relay_1"},"mode":"heat"})";
  write_file(this->file_of("summer"), text);
  this->reboot();
  ASSERT_TRUE(hub().is_running("winter"));
  // Set just before, still waiting to be written: it is, under the flag the file has.
  target(hub().entity_of("winter"), 23.f);

  Result result = hub().set_enabled("summer", true, true);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_FALSE(result.persisted);
  EXPECT_TRUE(hub().is_running("summer"));
  EXPECT_FALSE(hub().is_running("winter"));
  EXPECT_FALSE(hub().store().get("winter")->enabled);
  EXPECT_EQ("summer", hub().claimed_by("relay_1"));
  flush();
  EXPECT_EQ(text, read_file(this->file_of("summer")));
  const std::string winter = read_file(this->file_of("winter"));
  EXPECT_NE(std::string::npos, winter.find(R"("enabled":true)")) << winter;
  EXPECT_NE(std::string::npos, winter.find(R"("setpoint":23)")) << winter;

  this->reboot();
  EXPECT_TRUE(hub().is_running("winter"));
  EXPECT_FALSE(hub().is_running("summer"));
  EXPECT_FLOAT_EQ(23.f, hub().store().get("winter")->setpoint);
}

// Enabled already, it waited for the relay: there is no flag of its own to write, and the
// take-over still is not recorded.
TEST_F(Presets, ATakeOverByAWaitingNewerFileIsNotWrittenEither) {
  this->create(draft("Boiler"));
  write_file(this->file_of("summer"), R"({"version":3,"id":"summer","name":"Summer","sensor_id":"room",)"
                                      R"("heat":{"relay_id":"relay_1"},"mode":"heat"})");
  this->reboot();
  ASSERT_NE("", hub().waiting_reason("summer"));

  Result result = hub().set_enabled("summer", true, true);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_FALSE(result.persisted);
  EXPECT_TRUE(hub().is_running("summer"));
  EXPECT_NE(std::string::npos, read_file(this->file_of("boiler")).find(R"("enabled":true)"));
}

// One that waits for the relay is stored disabled by a take-over as the holder is: by a newer
// file's, in memory only.
TEST_F(Presets, ATakeOverByANewerFileLeavesAWaitersFileEnabled) {
  ClimateConfig attic = draft("Attic");
  attic.sensor_id = "gone";
  this->create(attic);
  const std::string text = R"({"version":3,"id":"summer","name":"Summer","enabled":false,"sensor_id":"room",)"
                           R"("heat":{"relay_id":"relay_1"},"mode":"heat"})";
  write_file(this->file_of("summer"), text);
  this->reboot();
  ASSERT_NE("", hub().waiting_reason("attic"));
  // Set just before, still waiting to be written: it is, under the flag the file has.
  ASSERT_TRUE(hub().set_setpoint("attic", 23.f).ok);

  Result result = hub().set_enabled("summer", true, true);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_FALSE(result.persisted);
  EXPECT_EQ(std::vector<std::string>{"attic"}, result.stopped);
  EXPECT_TRUE(hub().is_running("summer"));
  EXPECT_FALSE(hub().store().get("attic")->enabled);
  EXPECT_EQ("", hub().waiting_reason("attic"));
  flush();
  EXPECT_EQ(text, read_file(this->file_of("summer")));
  const std::string waiter = read_file(this->file_of("attic"));
  EXPECT_NE(std::string::npos, waiter.find(R"("enabled":true)")) << waiter;
  EXPECT_NE(std::string::npos, waiter.find(R"("setpoint":23)")) << waiter;

  this->reboot();
  EXPECT_TRUE(hub().store().get("attic")->enabled);
  EXPECT_FALSE(hub().store().get("summer")->enabled);
  EXPECT_FLOAT_EQ(23.f, hub().store().get("attic")->setpoint);
}

// A newer file whose name another thermostat has is renamed for this boot only.
TEST_F(Presets, ANewerFileRenamedAtBootIsNotWritten) {
  this->create(draft("Boiler"));
  const std::string text = R"({"version":9,"id":"kettle","name":"boiler","enabled":false,"sensor_id":"room",)"
                           R"("heat":{"relay_id":"relay_2"}})";
  write_file(this->file_of("kettle"), text);
  this->reboot();
  EXPECT_EQ("boiler 2", hub().store().get("kettle")->name);
  EXPECT_EQ(text, read_file(this->file_of("kettle")));
}

}  // namespace esphome::climate_hub::testing
