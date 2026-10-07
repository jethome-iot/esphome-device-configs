// A firmware without climate_hub: the editor offers no climate action and lists no thermostat,
// and the engine refuses a rule that names one, at save and at boot.
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "esphome/components/automations/automation_storage.h"
#include "esphome/components/automations/runtime_automation.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/dir_storage/dir_storage.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/web_automation_editor/web_automation_editor.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

#ifdef USE_CLIMATE_HUB
#error "this suite is the firmware without climate_hub; test.yaml must not load it"
#endif

namespace esphome::automations::testing {

namespace {

class FakeSwitch : public switch_::Switch {
 protected:
  void write_state(bool state) override { this->publish_state(state); }
};

struct Entities {
  binary_sensor::BinarySensor in1;
  FakeSwitch relay1;
};

// Registered once: App keeps the pointers for the life of the process.
Entities &entities() {
  static Entities *instance = [] {
    auto *e = new Entities();
    App.register_binary_sensor(&e->in1, "In 1", fnv1_hash("in_1"), 0);
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    return e;
  }();
  return *instance;
}

class TestEngine : public AutomationStorage {
 public:
  void forget() {
    this->automations_.clear();
    this->config_storage_.clear();
  }
};

class TestEditor : public web_automation_editor::WebAutomationEditor {
 public:
  using WebAutomationEditor::WebAutomationEditor;

 protected:
  void reboot_() override {}
};

const char *const ECO_AT_STARTUP =
    R"({"name":"Eco","triggers":[{"source":"startup"}],)"
    R"("actions":[{"source":"climate","type":"set_preset","climate":"living-room","preset":"eco"}]})";
const char *const PORCH_LIGHT =
    R"({"name":"Porch light","triggers":[{"source":"input","type":"press","object_id":"in_1"}],)"
    R"("actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})";

}  // namespace

class NoClimate : public ::testing::Test {
 protected:
  void SetUp() override {
    entities();
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->backend.set_base_path(folder);
    this->backend.setup();
    mkdir(this->rules().c_str(), 0755);
  }

  void TearDown() override {
    if (this->engine != nullptr)
      this->engine->forget();
    if (DIR *dir = opendir(this->rules().c_str())) {
      while (struct dirent *entry = readdir(dir)) {
        if (entry->d_name[0] != '.')
          remove((this->rules() + "/" + entry->d_name).c_str());
      }
      closedir(dir);
    }
    rmdir(this->rules().c_str());
    rmdir(this->backend.get_base_path().c_str());
  }

  // The engine loads what the folder holds by then, and the editor serves it. Engines outlive
  // the test: the entities keep a callback into every one that subscribed.
  void start() {
    static std::vector<std::unique_ptr<TestEngine>> all;
    all.push_back(std::make_unique<TestEngine>());
    this->engine = all.back().get();
    this->engine->set_storage(&this->backend);
    this->engine->setup();
    ASSERT_FALSE(this->engine->is_failed());
    this->editor = std::make_unique<TestEditor>(&this->base, this->engine);
    this->editor->setup();
  }

  std::string rules() const { return this->backend.get_base_path() + "/automations"; }
  std::string read(const std::string &file) const {
    std::ifstream in(this->rules() + "/" + file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }

  // The answer's status, and its body parsed into `json`.
  int call(http_method method, const std::string &route, const std::string &body, JsonDocument &json) {
    AsyncWebServerRequest request(method, "/automation-editor/api/" + route, body, "application/json");
    request.set_header("Host", "device.local");
    EXPECT_TRUE(this->base.get_server()->dispatch(request)) << route;
    EXPECT_EQ(deserializeJson(json, request.response_body), DeserializationError::Ok) << request.response_body;
    this->body = request.response_body;
    return request.response_code;
  }

  dir_storage::DirStorage backend;
  TestEngine *engine{nullptr};
  web_server_base::WebServerBase base;
  std::unique_ptr<TestEditor> editor;
  std::string body;
};

TEST_F(NoClimate, TheSchemaOffersNoClimateAction) {
  start();
  JsonDocument schema;
  ASSERT_EQ(this->call(HTTP_GET, "schema", "", schema), 200);
  std::vector<std::string> actions;
  for (JsonObject entry : schema["actions"].as<JsonArray>())
    actions.push_back(entry["type"].as<std::string>());
  EXPECT_EQ(actions, (std::vector<std::string>{"switch", "delay"}));
  EXPECT_EQ(schema["cron_presets"].size(), 6u) << "the rest of the catalog is whole";
  EXPECT_EQ(this->body.find("climate"), std::string::npos) << this->body;
}

TEST_F(NoClimate, EntitiesListNoThermostat) {
  start();
  JsonDocument entities;
  ASSERT_EQ(this->call(HTTP_GET, "entities", "", entities), 200);
  EXPECT_EQ(this->body, R"({"binary_sensors":[{"object_id":"in_1","name":"In 1"}],"sensors":[],)"
                        R"("switches":[{"object_id":"relay_1","name":"Relay 1"}],"climates":[]})");
}

TEST_F(NoClimate, ASaveNamingAThermostatIsRefused) {
  start();
  JsonDocument reply;
  EXPECT_EQ(this->call(HTTP_POST, "save", ECO_AT_STARTUP, reply), 400);
  EXPECT_EQ(reply["error"].as<std::string>(), R"(Action 1: thermostat "living-room" not found)");
  EXPECT_EQ(this->engine->configs().size(), 0u);

  // The engine still runs everything else.
  EXPECT_EQ(this->call(HTTP_POST, "save", PORCH_LIGHT, reply), 200) << this->body;
  entities().in1.publish_state(true);
  EXPECT_TRUE(entities().relay1.state);
  entities().in1.publish_state(false);
  entities().relay1.turn_off();
}

TEST_F(NoClimate, ARuleFileNamingAThermostatIsKeptButNotBuilt) {
  const std::string text = R"({"id":1,"name":"Eco","enabled":true,"mode":"single","triggers":[{"source":"startup"}],)"
                           R"("actions":[{"source":"climate","type":"turn_off","climate":"living-room"}]})";
  {
    std::ofstream out(this->rules() + "/eco.json");
    out << text;
  }
  start();
  JsonDocument list;
  ASSERT_EQ(this->call(HTTP_GET, "list", "", list), 200);
  ASSERT_EQ(list["automations"].size(), 1u);
  EXPECT_FALSE(list["automations"][0]["built"].as<bool>());
  EXPECT_EQ(list["automations"][0]["build_error"].as<std::string>(), R"(Action 1: thermostat "living-room" not found)");
  EXPECT_EQ(this->read("eco.json"), text);
}

}  // namespace esphome::automations::testing
