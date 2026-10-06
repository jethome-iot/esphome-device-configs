// Rules that act on climate_hub's thermostats: the action format, the five types against the
// real hub, the wait for the next loop pass, and fail-closed at save, at boot and as the hub's
// thermostats come and go.
#include "common.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include "esphome/components/climate_hub/climate_hub.h"

namespace esphome::automations::testing {

using climate_hub::ClimateConfig;
using climate_hub::HubMode;
using climate_hub::PresetConfig;

namespace {

void clear_folder(const std::string &path) {
  DIR *dir = opendir(path.c_str());
  if (dir == nullptr)
    return;
  while (struct dirent *entry = readdir(dir)) {
    if (entry->d_name[0] != '.')
      remove((path + "/" + entry->d_name).c_str());
  }
  closedir(dir);
}

// The one hub of the process: App has room for the one pool test.yaml declares. Over a folder
// of its own, emptied first so a crashed run leaves nothing behind.
climate_hub::ClimateHub &hub() {
  static climate_hub::ClimateHub *instance = [] {
    entities();
    static FakeStorage storage;
    mkdir(".storage", 0755);
    mkdir(".storage/thermostats", 0755);
    storage.path = ".storage/thermostats";
    clear_folder(storage.path + "/climates");
    auto *h = new climate_hub::ClimateHub();
    h->set_storage(&storage);
    h->set_max_controllers(2);
    h->setup();
    return h;
  }();
  return *instance;
}

PresetConfig preset(const char *name, float setpoint, optional<HubMode> mode = nullopt) {
  PresetConfig p;
  p.name = name;
  p.setpoint = setpoint;
  p.mode = mode;
  return p;
}

// Heats on `boiler` from `room`, at 21, with three presets: eco, comfort and away, which turns
// it off. Only one may run: `boiler` is its relay.
ClimateConfig thermostat(const char *name = "Living room", bool enabled = true) {
  ClimateConfig c;
  c.name = name;
  c.enabled = enabled;
  c.sensor_id = "room";
  c.heat.relay_id = "boiler";
  c.mode = HubMode::HEAT;
  c.setpoint = 21.f;
  c.presets = {preset("Eco", 18.f), preset("Comfort", 22.f), preset("Away", 12.f, HubMode::OFF)};
  return c;
}

const ClimateConfig &stored(const std::string &id) { return *hub().store().get(id); }

// A rule fired on a startup trigger, doing `actions`.
std::string at_startup(const std::string &actions) {
  return R"({"name":"Rule","triggers":[{"source":"startup"}],"actions":[)" + actions + "]}";
}

const char *const TURN_OFF = R"({"source":"climate","type":"turn_off","climate":"living-room"})";
const char *const TURN_ON = R"({"source":"climate","type":"turn_on","climate":"living-room"})";
const char *const ECO = R"({"source":"climate","type":"set_preset","climate":"living-room","preset":"eco"})";
// Comfort while In 1 is on, off while it is off.
const char *const FOLLOW_IN_1 =
    R"({"name":"Follow","triggers":[{"source":"input","type":"state_change","object_id":"in_1"}],)"
    R"("actions":[{"source":"climate","type":"follow","climate":"living-room",)"
    R"("on":{"type":"set_preset","preset":"comfort"},"off":{"type":"turn_off"}}]})";

}  // namespace

// --- The format ---

TEST(ClimateActionConfig, EveryTypeRoundTrips) {
  for (
      const char *json : {
          R"({"source":"climate","type":"turn_on","climate":"living-room"})",
          R"({"source":"climate","type":"turn_off","climate":"living-room"})",
          R"({"source":"climate","type":"set_preset","climate":"living-room","preset":"eco"})",
          R"({"source":"climate","type":"set_target","climate":"living-room","target":22.5})",
          R"({"source":"climate","type":"follow","climate":"living-room","on":{"type":"set_preset","preset":"comfort"},)"
          R"("off":{"type":"set_preset","preset":"eco"}})",
          R"({"source":"climate","type":"follow","climate":"attic","on":{"type":"turn_on"},"off":{"type":"turn_off"}})",
      }) {
    ActionConfig action;
    ASSERT_TRUE(load(json, action)) << json;
    EXPECT_EQ(action.source, SourceAction::CLIMATE);
    EXPECT_EQ(dump(action), json);
  }
}

TEST(ClimateActionConfig, ReadsWhatItSays) {
  ActionConfig action;
  ASSERT_TRUE(load(R"({"source":"climate","type":"follow","climate":"attic","on":{"type":"turn_on"},)"
                   R"("off":{"type":"set_preset","preset":"eco","target":3}})",
                   action));
  EXPECT_EQ(action.climate.climate, "attic");
  EXPECT_EQ(action.climate.step.type, TypeClimateAction::FOLLOW);
  EXPECT_EQ(action.climate.on.type, TypeClimateAction::TURN_ON);
  EXPECT_EQ(action.climate.off.type, TypeClimateAction::SET_PRESET);
  EXPECT_EQ(action.climate.off.preset, "eco");
  EXPECT_TRUE(std::isnan(action.climate.off.target)) << "a word a step does not take is not read";

  ASSERT_TRUE(load(R"({"source":"climate","type":"set_target","climate":"attic","target":19})", action));
  EXPECT_FLOAT_EQ(action.climate.step.target, 19.f);
}

// Each refusal fails the whole rule at the parser, as any other word the engine does not know.
TEST(ClimateActionConfig, RefusesWhatItCannotRun) {
  struct Case {
    const char *json;
    const char *logged;
  };
  for (
      const Case &c : {
          Case{R"({"source":"climate","type":"turn_on"})", "Missing climate"},
          Case{R"({"source":"climate","type":"turn_on","climate":""})", "Missing climate"},
          Case{R"({"source":"climate","type":"turn_on","climate":7})", "Missing climate"},
          Case{R"({"source":"climate","climate":"attic"})", "Missing type"},
          Case{R"({"source":"climate","type":"turn_up","climate":"attic"})", "Unknown type 'turn_up'"},
          Case{R"({"source":"climate","type":"none","climate":"attic"})", "Unknown type 'none'"},
          Case{R"({"source":"climate","type":"set_preset","climate":"attic"})", "Missing preset"},
          Case{R"({"source":"climate","type":"set_preset","climate":"attic","preset":""})", "Missing preset"},
          Case{R"({"source":"climate","type":"set_preset","climate":"attic","preset":3})", "Missing preset"},
          Case{R"({"source":"climate","type":"set_target","climate":"attic"})", "Missing target"},
          Case{R"({"source":"climate","type":"set_target","climate":"attic","target":"warm"})", "Missing target"},
          Case{R"({"source":"climate","type":"follow","climate":"attic","off":{"type":"turn_off"}})", "Missing on"},
          Case{R"({"source":"climate","type":"follow","climate":"attic","on":{"type":"turn_on"}})", "Missing off"},
          Case{R"({"source":"climate","type":"follow","climate":"attic","on":"turn_on","off":{"type":"turn_off"}})",
               "Missing on"},
          Case{R"({"source":"climate","type":"follow","climate":"attic","on":{"type":"set_target","target":20},)"
               R"("off":{"type":"turn_off"}})",
               "A follow takes turn_on, turn_off or set_preset, not 'set_target'"},
          Case{R"({"source":"climate","type":"follow","climate":"attic","on":{"type":"turn_on"},)"
               R"("off":{"type":"follow"}})",
               "A follow takes turn_on, turn_off or set_preset, not 'follow'"},
          Case{
              R"({"source":"climate","type":"follow","climate":"attic","on":{"type":"heat"},"off":{"type":"turn_off"}})",
              "Unknown type 'heat'"},
          Case{R"({"source":"climate","type":"follow","climate":"attic","on":{"type":"set_preset"},)"
               R"("off":{"type":"turn_off"}})",
               "Missing preset"},
      }) {
    LogCapture::instance().clear();
    ActionConfig action;
    EXPECT_FALSE(load(c.json, action)) << c.json;
    EXPECT_TRUE(LogCapture::instance().has(LogCapture::instance().errors, c.logged)) << c.json;
  }
}

TEST(ClimateActionConfig, ARuleWithThemRoundTrips) {
  const std::string json =
      R"({"id":3,"name":"Heat on arrival","enabled":true,"mode":"restart","triggers":[{"source":"startup"}],)"
      R"("condition":{"type":"input","object_id":"in_1","state":"true"},)"
      R"("actions":[{"source":"climate","type":"set_preset","climate":"living-room","preset":"comfort"},)"
      R"({"source":"delay","delay_ms":60000},{"source":"climate","type":"set_target","climate":"living-room","target":23}],)"
      R"("else_actions":[{"source":"climate","type":"turn_off","climate":"living-room"}]})";
  entities();
  AutomationConfig config;
  ASSERT_TRUE(load(json.c_str(), config));
  EXPECT_EQ(dump(config), json);
}

// --- Against the hub ---

class ClimateRules : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_entities();
    LogCapture::instance().clear();
    hub();
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->backend.path = folder;
    mkdir(this->rules().c_str(), 0755);
    climate_hub::Result created = hub().create(thermostat());
    ASSERT_TRUE(created.ok) << created.error;
    ASSERT_EQ(created.id, "living-room");
    ASSERT_TRUE(hub().is_running("living-room"));
    this->engine = &this->new_engine();
  }

  void TearDown() override {
    for (FakeEngine *e : this->engines)
      e->forget();
    std::vector<std::string> ids;
    for (const auto &config : hub().store().all())
      ids.push_back(config->id);
    for (const std::string &id : ids)
      hub().remove(id);
    clear_folder(this->rules());
    rmdir(this->rules().c_str());
    rmdir(this->backend.path.c_str());
  }

  // Engines outlive the test: the hub and the entities keep a callback into every one that set
  // up. forget() empties them, so later tests see nothing fire.
  FakeEngine &new_engine() {
    static std::vector<std::unique_ptr<FakeEngine>> all;
    all.push_back(std::make_unique<FakeEngine>());
    FakeEngine &e = *all.back();
    e.set_storage(&this->backend);
    this->engines.push_back(&e);
    return e;
  }

  std::string rules() const { return this->backend.path + "/automations"; }
  void write(const std::string &file, const std::string &json) {
    std::ofstream out(this->rules() + "/" + file);
    out << json;
  }
  std::string read(const std::string &file) const {
    std::ifstream in(this->rules() + "/" + file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }
  bool exists(const std::string &file) const {
    struct stat st;
    return stat((this->rules() + "/" + file).c_str(), &st) == 0;
  }
  AutomationConfig rule(const std::string &json) {
    AutomationConfig config;
    EXPECT_TRUE(load(json.c_str(), config)) << json;
    return config;
  }
  // A Save of the living room with `presets` in place of its own.
  void save_presets(std::vector<PresetConfig> presets) {
    ClimateConfig doc = stored("living-room");
    doc.presets = std::move(presets);
    climate_hub::Result saved = hub().update("living-room", doc);
    ASSERT_TRUE(saved.ok) << saved.error;
  }
  // The loaded rule, by index, and whether it is built.
  const AutomationConfig &config(size_t index) const { return this->engine->configs().get_all_configs()[index]; }
  bool built(size_t index) { return this->engine->rule(index) != nullptr; }

  FakeStorage backend;
  FakeEngine *engine{nullptr};
  std::vector<FakeEngine *> engines;
  Entities &e = entities();
};

TEST_F(ClimateRules, BuildsWhenTheThermostatAndItsPresetsAreThere) {
  std::string error = "untouched";
  EXPECT_NE(RuntimeAutomation::build(engine, rule(at_startup(ECO)), &error), nullptr);
  EXPECT_EQ(error, "untouched");
  EXPECT_NE(build_rule(*engine, FOLLOW_IN_1), nullptr);
}

TEST_F(ClimateRules, ARuleNamingWhatIsNotThereIsNotBuilt) {
  struct Case {
    std::string json;
    const char *error;
  };
  for (const Case &c : {
           Case{at_startup(R"({"source":"climate","type":"turn_on","climate":"attic"})"),
                R"(Action 1: thermostat "attic" not found)"},
           Case{at_startup(std::string(TURN_ON) +
                           R"(,{"source":"climate","type":"set_preset","climate":"living-room","preset":"boost"})"),
                R"(Action 2: thermostat "living-room" has no preset "boost")"},
           Case{at_startup(
                    R"({"source":"climate","type":"follow","climate":"living-room",)"
                    R"("on":{"type":"set_preset","preset":"comfort"},"off":{"type":"set_preset","preset":"night"}})"),
                R"(Action 1: thermostat "living-room" has no preset "night")"},
           Case{R"({"name":"R","triggers":[{"source":"startup"}],"condition":{"type":"input","object_id":"in_1"},)"
                R"("actions":[],"else_actions":[{"source":"climate","type":"turn_off","climate":"Living room"}]})",
                R"(Else action 1: thermostat "Living room" not found)"},
       }) {
    std::string error;
    EXPECT_EQ(RuntimeAutomation::build(engine, rule(c.json), &error), nullptr) << c.json;
    EXPECT_EQ(error, c.error);
  }
}

// The other reasons a rule is not built, in the words the editor shows.
TEST_F(ClimateRules, EveryReasonARuleIsNotBuiltIsSaid) {
  struct Case {
    std::string json;
    const char *error;
  };
  for (
      const Case &c : {
          Case{R"({"name":"R","triggers":[],"actions":[]})", "No triggers"},
          Case{
              R"({"name":"R","triggers":[{"source":"startup"},{"source":"input","type":"press","object_id":"x"}],"actions":[]})",
              "Trigger 2: input not found"},
          Case{R"({"name":"R","triggers":[{"source":"switch","type":"turn_on","object_id":"x"}],"actions":[]})",
               "Trigger 1: switch not found"},
          Case{
              R"({"name":"R","triggers":[{"source":"temperature","type":"above","object_id":"x","threshold":1}],"actions":[]})",
              "Trigger 1: sensor not found"},
          Case{R"({"name":"R","triggers":[{"source":"cron","cron":"0 * * * * *"}],"actions":[]})",
               "Trigger 1: cron needs a time source"},
          Case{
              R"({"name":"R","triggers":[{"source":"startup"}],"condition":{"type":"and","conditions":[)"
              R"({"type":"input","object_id":"in_1"},{"type":"temperature","object_id":"x","temperature_type":"above","threshold":1}]},"actions":[]})",
              "Condition: sensor not found"},
          Case{
              R"({"name":"R","triggers":[{"source":"startup"}],"condition":{"type":"input","object_id":"x"},"actions":[]})",
              "Condition: input not found"},
          Case{at_startup(R"({"source":"delay","delay_ms":1},{"source":"switch","type":"turn_on","object_id":"x"})"),
               "Action 2: switch not found"},
      }) {
    std::string error;
    EXPECT_EQ(RuntimeAutomation::build(engine, rule(c.json), &error), nullptr) << c.json;
    EXPECT_EQ(error, c.error);
  }
}

TEST_F(ClimateRules, TurnOffAndTurnOn) {
  auto off = build_rule(*engine, at_startup(TURN_OFF).c_str());
  auto on = build_rule(*engine, at_startup(TURN_ON).c_str());
  ASSERT_NE(off, nullptr);
  ASSERT_NE(on, nullptr);
  off->on_startup();
  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("living-room").mode, HubMode::OFF);
  EXPECT_FALSE(off->is_running());

  on->on_startup();
  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("living-room").mode, HubMode::HEAT) << "the mode it was in before";
}

TEST_F(ClimateRules, SetPreset) {
  auto rule = build_rule(*engine, at_startup(ECO).c_str());
  ASSERT_NE(rule, nullptr);
  rule->on_startup();
  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("living-room").active_preset, "eco");
  EXPECT_FLOAT_EQ(stored("living-room").setpoint, 18.f);
}

TEST_F(ClimateRules, SetTargetInsideTheThermostatsRange) {
  auto warm = build_rule(
      *engine, at_startup(R"({"source":"climate","type":"set_target","climate":"living-room","target":23.5})").c_str());
  auto hot = build_rule(
      *engine, at_startup(R"({"source":"climate","type":"set_target","climate":"living-room","target":99})").c_str());
  warm->on_startup();
  ASSERT_TRUE(engine->fire_next());
  EXPECT_FLOAT_EQ(stored("living-room").setpoint, 23.5f);
  hot->on_startup();
  ASSERT_TRUE(engine->fire_next());
  EXPECT_FLOAT_EQ(stored("living-room").setpoint, 45.f) << "clamped to visual.max_temperature";
}

TEST_F(ClimateRules, FollowGoesBothWays) {
  auto rule = build_rule(*engine, FOLLOW_IN_1);
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, true);
  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("living-room").active_preset, "comfort");
  EXPECT_FLOAT_EQ(stored("living-room").setpoint, 22.f);
  EXPECT_EQ(stored("living-room").mode, HubMode::HEAT);

  rule->on_binary_sensor(&e.in1, false);
  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("living-room").mode, HubMode::OFF);

  // A follow on/off pair: back on in the mode it left.
  auto power =
      build_rule(*engine, R"({"name":"Power","triggers":[{"source":"input","type":"state_change","object_id":"in_2"}],)"
                          R"("actions":[{"source":"climate","type":"follow","climate":"living-room",)"
                          R"("on":{"type":"turn_on"},"off":{"type":"turn_off"}}]})");
  power->on_binary_sensor(&e.in2, true);
  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("living-room").mode, HubMode::HEAT);
}

// A trigger that carries no state gives a follow nothing to follow, as for a switch.
TEST_F(ClimateRules, FollowWithoutAStateDoesNothing) {
  auto rule = build_rule(*engine, at_startup(R"({"source":"climate","type":"follow","climate":"living-room",)"
                                             R"("on":{"type":"turn_on"},"off":{"type":"turn_off"}})")
                                      .c_str());
  rule->on_startup();
  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("living-room").mode, HubMode::HEAT);
  EXPECT_FALSE(rule->is_running());
}

// Off the sensor's callback: the switch before moves at once, the thermostat and everything
// after it on the next loop pass, together.
TEST_F(ClimateRules, AThermostatMovesOnTheNextLoopPass) {
  auto rule = build_rule(
      *engine,
      R"({"name":"Hot","triggers":[{"source":"temperature","type":"above","object_id":"temp","threshold":25}],)"
      R"("actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},)"
      R"({"source":"climate","type":"turn_off","climate":"living-room"},)"
      R"({"source":"climate","type":"set_target","climate":"living-room","target":19},)"
      R"({"source":"switch","type":"turn_on","object_id":"relay_2"}]})");
  ASSERT_NE(rule, nullptr);
  rule->on_sensor(&e.temp, 30.f);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(stored("living-room").mode, HubMode::HEAT);
  EXPECT_FALSE(e.relay2.state);
  ASSERT_EQ(engine->delays.size(), 1u);
  EXPECT_EQ(engine->delays[0].ms, 0u);
  EXPECT_TRUE(rule->is_running());

  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("living-room").mode, HubMode::OFF);
  EXPECT_FLOAT_EQ(stored("living-room").setpoint, 19.f);
  EXPECT_TRUE(e.relay2.state);
  EXPECT_FALSE(rule->is_running());
  EXPECT_TRUE(engine->delays.empty());
}

// After a delay the run is on a loop pass already: the thermostat moves then, not one later.
TEST_F(ClimateRules, AfterADelayItMovesAtOnce) {
  auto rule = build_rule(*engine, at_startup(R"({"source":"delay","delay_ms":500},)" + std::string(TURN_OFF)).c_str());
  rule->on_startup();
  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("living-room").mode, HubMode::OFF);
  EXPECT_TRUE(engine->delays.empty());
}

// A rule stopped or restarted before the pass drops the step, as it drops a delay.
TEST_F(ClimateRules, AStopDropsTheWaitingStep) {
  auto rule = build_rule(*engine, at_startup(TURN_OFF).c_str());
  rule->on_startup();
  rule->stop();
  EXPECT_TRUE(engine->delays.empty());
  EXPECT_EQ(stored("living-room").mode, HubMode::HEAT);
}

// The hub dropped the thermostat between the build and the pass: the step is logged and the
// run goes on.
TEST_F(ClimateRules, AThermostatGoneByThePassIsLoggedAndSkipped) {
  auto rule = build_rule(
      *engine,
      at_startup(std::string(TURN_OFF) + R"(,{"source":"switch","type":"turn_on","object_id":"relay_1"})").c_str());
  rule->on_startup();
  ASSERT_TRUE(hub().remove("living-room").ok);
  ASSERT_TRUE(engine->fire_next());
  EXPECT_TRUE(LogCapture::instance().has(LogCapture::instance().warnings,
                                         "Automation 'Rule': thermostat 'living-room': Thermostat not found"));
  EXPECT_TRUE(e.relay1.state);
}

// --- Fail closed: at save, at boot, and as the hub changes ---

TEST_F(ClimateRules, ASaveNamingWhatIsNotThereIsRefusedWithTheReason) {
  engine->setup();
  std::string error;
  EXPECT_EQ(
      engine->add_automation(rule(at_startup(R"({"source":"climate","type":"turn_on","climate":"attic"})")), &error),
      0u);
  EXPECT_EQ(error, R"(Action 1: thermostat "attic" not found)");
  EXPECT_FALSE(exists("rule.json"));

  const uint32_t id = engine->add_automation(rule(at_startup(ECO)), &error);
  ASSERT_NE(id, 0u);
  EXPECT_TRUE(config(0).build_error.empty());
  EXPECT_FALSE(engine->update_automation(
      id, rule(at_startup(R"({"source":"climate","type":"set_preset","climate":"living-room","preset":"boost"})")),
      &error));
  EXPECT_EQ(error, R"(Action 1: thermostat "living-room" has no preset "boost")");
  EXPECT_NE(read("rule.json").find(R"("preset":"eco")"), std::string::npos) << "the file is as it was";

  // A refusal for another reason says nothing of what is missing.
  error = "untouched";
  EXPECT_FALSE(engine->update_automation(id + 1, rule(at_startup(ECO)), &error));
  EXPECT_EQ(error, "");
}

TEST_F(ClimateRules, AtBootARuleNamingWhatIsNotThereIsKeptButNotBuilt) {
  const std::string text = R"({"id":1,"name":"Attic","enabled":true,"mode":"single","triggers":[{"source":"startup"}],)"
                           R"("actions":[{"source":"climate","type":"turn_on","climate":"attic"}]})";
  write("attic.json", text);
  engine->setup();
  ASSERT_EQ(engine->configs().size(), 1u);
  EXPECT_FALSE(built(0));
  EXPECT_EQ(config(0).build_error, R"(Action 1: thermostat "attic" not found)");
  EXPECT_EQ(read("attic.json"), text);

  LogCapture::instance().clear();
  engine->dump_config();
  EXPECT_TRUE(LogCapture::instance().has(LogCapture::instance().lines,
                                         R"('Attic' enabled (not built: Action 1: thermostat "attic" not found))"));
  EXPECT_TRUE(LogCapture::instance().has(LogCapture::instance().lines, "Action: climate turn_on 'attic'"));
}

// A file the engine rewrites at boot, to restamp its id say, keeps the thermostat it names:
// the id is a string in memory, not an entity's hash.
TEST_F(ClimateRules, ARepairAtBootKeepsTheThermostatItNames) {
  write("elsewhere.json", R"({"name":"Attic","triggers":[{"source":"startup"}],)"
                          R"("actions":[{"source":"climate","type":"turn_on","climate":"attic"}]})");
  engine->setup();
  EXPECT_FALSE(exists("elsewhere.json"));
  EXPECT_NE(read("attic.json").find(R"("climate":"attic")"), std::string::npos);
}

TEST_F(ClimateRules, ACreateBuildsTheRulesThatNamedIt) {
  write("attic.json", R"({"id":1,"name":"Attic","triggers":[{"source":"startup"}],)"
                      R"("actions":[{"source":"climate","type":"set_preset","climate":"attic","preset":"eco"}]})");
  engine->setup();
  ASSERT_FALSE(built(0));

  climate_hub::Result created = hub().create(thermostat("Attic", false));
  ASSERT_TRUE(created.ok) << created.error;
  ASSERT_TRUE(built(0));
  EXPECT_TRUE(config(0).build_error.empty());
  engine->rule(0)->on_startup();
  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("attic").active_preset, "eco");
}

TEST_F(ClimateRules, ARemovalDropsTheRulesThatNameIt) {
  engine->setup();
  ASSERT_NE(engine->add_automation(rule(at_startup(TURN_OFF))), 0u);
  ASSERT_NE(engine->add_automation(rule(R"({"name":"Other","triggers":[{"source":"startup"}],"actions":[]})")), 0u);
  ASSERT_TRUE(built(0));

  ASSERT_TRUE(hub().remove("living-room").ok);
  EXPECT_FALSE(built(0));
  EXPECT_EQ(config(0).build_error, R"(Action 1: thermostat "living-room" not found)");
  EXPECT_TRUE(exists("rule.json"));
  EXPECT_TRUE(built(1)) << "a rule that names no thermostat is left alone";
  EXPECT_TRUE(LogCapture::instance().has(LogCapture::instance().warnings, "Automation 'Rule' is not built any more"));

  ASSERT_TRUE(hub().create(thermostat()).ok);
  EXPECT_TRUE(built(0)) << "back once it is there again";
}

TEST_F(ClimateRules, APresetChangeRebuildsTheRulesThatNameIt) {
  engine->setup();
  ASSERT_NE(engine->add_automation(rule(at_startup(ECO))), 0u);

  this->save_presets({preset("Comfort", 22.f)});
  EXPECT_FALSE(built(0));
  EXPECT_EQ(config(0).build_error, R"(Action 1: thermostat "living-room" has no preset "eco")");

  this->save_presets({preset("Comfort", 22.f), preset("Eco", 17.f)});
  ASSERT_TRUE(built(0));
  engine->rule(0)->on_startup();
  ASSERT_TRUE(engine->fire_next());
  EXPECT_FLOAT_EQ(stored("living-room").setpoint, 17.f);
}

// A change that leaves the rule buildable leaves it running: a run waiting out a delay goes on.
TEST_F(ClimateRules, ARuleTheChangeLeavesWhole) {
  engine->setup();
  ASSERT_NE(engine->add_automation(rule(at_startup(R"({"source":"delay","delay_ms":1000},)" + std::string(ECO)))), 0u);
  RuntimeAutomation *before = engine->rule(0);
  before->on_startup();
  ASSERT_TRUE(before->is_running());

  this->save_presets({preset("Eco", 18.f), preset("Night", 19.f)});
  ASSERT_TRUE(hub().create(thermostat("Attic", false)).ok);
  EXPECT_EQ(engine->rule(0), before);
  EXPECT_TRUE(before->is_running());
  ASSERT_TRUE(engine->fire_next());
  EXPECT_EQ(stored("living-room").active_preset, "eco");
}

// The hub changed from inside a rule's own action, a lambda behind its switch say: the rebuild
// waits for the next loop pass instead of pulling the rule from under its run.
TEST_F(ClimateRules, AChangeFromInsideARuleWaitsForTheLoop) {
  engine->setup();
  ASSERT_NE(
      engine->add_automation(rule(R"({"name":"Rule","triggers":[{"source":"input","type":"press","object_id":"in_1"}],)"
                                  R"("actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},)"
                                  R"({"source":"climate","type":"turn_off","climate":"living-room"}]})")),
      0u);
  e.relay1.on_change = []() { hub().remove("living-room"); };
  e.in1.publish_state(true);
  EXPECT_TRUE(built(0)) << "still the rule whose run is going";
  EXPECT_EQ(hub().store().get("living-room"), nullptr);

  App.scheduler.call(millis());
  EXPECT_FALSE(built(0));
  EXPECT_TRUE(engine->delays.empty()) << "its waiting step went with it";
}

}  // namespace esphome::automations::testing
