#pragma once
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/automations/automation_storage.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/dir_storage/dir_storage.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/web_automation_editor/web_automation_editor.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

namespace esphome::web_automation_editor::testing {

class FakeSwitch : public switch_::Switch {
 protected:
  void write_state(bool state) override { this->publish_state(state); }
};

// The entities the API may list and the rules may name. Registered once: App keeps the
// pointers for the life of the process.
struct Entities {
  binary_sensor::BinarySensor in1;
  sensor::Sensor temp;
  FakeSwitch relay1;
  FakeSwitch hidden;
};

inline Entities &entities() {
  static Entities *instance = [] {
    auto *e = new Entities();
    App.register_binary_sensor(&e->in1, "In 1", fnv1_hash("in_1"), 0);
    // Units are indices into the table codegen builds from test.yaml, where "°C" is the only one.
    App.register_sensor(&e->temp, "Temp", fnv1_hash("temp"), 1u << ENTITY_FIELD_UOM_SHIFT);
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    App.register_switch(&e->hidden, "Hidden", fnv1_hash("hidden"), 1u << ENTITY_FIELD_INTERNAL_SHIFT);
    return e;
  }();
  return *instance;
}

// An engine a test can empty: the entities keep a callback into every engine that subscribed,
// so engines are never destroyed, only forgotten.
class TestEngine : public automations::AutomationStorage {
 public:
  void forget() {
    this->automations_.clear();
    this->config_storage_.clear();
  }
};

// The editor with the reboot held back: App.safe_reboot() would end the process.
class TestEditor : public WebAutomationEditor {
 public:
  using WebAutomationEditor::WebAutomationEditor;
  int reboots{0};

 protected:
  void reboot_() override { this->reboots++; }
};

// One answered request: whether a handler claimed it, and what it said.
struct Reply {
  bool claimed{false};
  int code{0};
  std::string type;
  std::string body;
  JsonDocument json;

  JsonVariant operator[](const char *key) { return this->json[key]; }
  std::string error() { return this->json["error"] | ""; }
  std::string message() { return this->json["message"] | ""; }
};

static const char *const PORCH_LIGHT =
    R"({"name":"Porch light","enabled":true,"mode":"single","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})";
static const char *const FAN =
    R"({"name":"Fan \"on\" when hot","enabled":false,"mode":"restart","triggers":[{"source":"temperature","type":"above","object_id":"temp","threshold":28}],"condition":{"type":"input","object_id":"in_1","state":"true"},"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},{"source":"delay","delay_ms":5000}],"else_actions":[{"source":"switch","type":"turn_off","object_id":"relay_1"}]})";

// The editor over a real engine over a temporary directory, reached through the harness's
// web_server_base stand-in exactly as a request from the network would reach it.
class Editor : public ::testing::Test {
 protected:
  void SetUp() override {
    entities();
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->backend.set_base_path(folder);
    this->backend.setup();
    this->engine = &this->new_engine();
    this->engine->set_storage(&this->backend);
    this->engine->setup();
    ASSERT_FALSE(this->engine->is_failed());
    this->editor = std::make_unique<TestEditor>(&this->base, this->engine);
    this->editor->setup();
  }

  void TearDown() override {
    this->engine->forget();
    for (const std::string &name : this->files())
      remove((this->rules() + "/" + name).c_str());
    rmdir(this->rules().c_str());
    rmdir(this->backend.get_base_path().c_str());
  }

  TestEngine &new_engine() {
    static std::vector<std::unique_ptr<TestEngine>> all;
    all.push_back(std::make_unique<TestEngine>());
    return *all.back();
  }

  std::string rules() const { return this->backend.get_base_path() + "/automations"; }

  std::vector<std::string> files() const {
    std::vector<std::string> names;
    DIR *dir = opendir(this->rules().c_str());
    if (dir == nullptr)
      return names;
    while (struct dirent *entry = readdir(dir)) {
      if (entry->d_name[0] != '.')
        names.emplace_back(entry->d_name);
    }
    closedir(dir);
    return names;
  }

  // @p origin is what a page on another site would carry; nullptr is a client that sends none.
  Reply call(http_method method, const std::string &route, const std::string &body = "",
             const std::string &content_type = "application/json", const char *origin = nullptr) {
    AsyncWebServerRequest request(method, "/automation-editor/api/" + route, body, content_type);
    request.set_header("Host", HOST);
    if (origin != nullptr)
      request.set_header("Origin", origin);
    Reply reply;
    reply.claimed = this->base.get_server()->dispatch(request);
    reply.code = request.response_code;
    reply.type = request.response_type;
    reply.body = request.response_body;
    EXPECT_LE(request.responses, 1) << route << " was answered twice";
    if (!reply.body.empty() && reply.type == "application/json")
      EXPECT_EQ(deserializeJson(reply.json, reply.body), DeserializationError::Ok) << reply.body;
    return reply;
  }
  Reply get(const std::string &route) { return this->call(HTTP_GET, route); }
  Reply post(const std::string &route, const std::string &body = "") { return this->call(HTTP_POST, route, body); }

  // Creates a rule and returns its id, 0 when the API refused it.
  uint32_t create(const char *json) {
    Reply reply = this->post("save", json);
    EXPECT_EQ(reply.code, 200) << reply.body;
    return reply["id"] | 0;
  }

  // The Host every case is addressed to; a page on another site says so by carrying an Origin
  // that is not this.
  static constexpr const char *HOST = "device.local";

  dir_storage::DirStorage backend;
  TestEngine *engine{nullptr};
  web_server_base::WebServerBase base;
  std::unique_ptr<TestEditor> editor;
};

}  // namespace esphome::web_automation_editor::testing
