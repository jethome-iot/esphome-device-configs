// A firmware without climate_hub: a relay schedule runs, a thermostat target is missing and
// says why, and no thermostat is offered, by the engine or over HTTP.
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/schedules/schedule_engine.h"
#include "esphome/components/schedules/week_clock.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/web_schedule_editor/web_schedule_editor.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

#ifdef USE_CLIMATE_HUB
#error "this suite is the firmware without climate_hub; test.yaml must not load it"
#endif

namespace esphome::schedules::testing {

namespace {

class FakeSwitch : public switch_::Switch {
 protected:
  void write_state(bool state) override { this->publish_state(state); }
};

class FakeStorage : public filesystem_storage_abstract::FilesystemStorageAbstract {
 public:
  std::string path;
  bool is_mounted() const override { return true; }
  const std::string &get_base_path() const override { return this->path; }
  const char *get_filesystem_type() const override { return "Directory"; }
  bool request_format() override { return false; }
};

// The local clock is `local`, 0 for none valid yet.
class TestEngine : public ScheduleEngine {
 public:
  int64_t local{0};
  void tick() { this->tick_(); }
  void forget() { this->entries_.clear(); }

 protected:
  ESPTime clock_now_() override { return this->local == 0 ? ESPTime{} : ESPTime::from_epoch_utc(this->local); }
  uint64_t mono_ms_() override { return 1000; }
};

FakeSwitch &relay1() {
  static FakeSwitch *instance = [] {
    auto *sw = new FakeSwitch();
    App.register_switch(sw, "Relay 1", fnv1_hash("relay_1"), 0);
    return sw;
  }();
  return *instance;
}

// Monday 2026-10-26, 09:00.
int64_t monday_nine() { return days_from_civil(2026, 10, 26) * 86400 + 9 * 3600; }

class NoClimate : public ::testing::Test {
 protected:
  void SetUp() override {
    relay1().publish_state(false);
    mkdir(".storage", 0755);
    char folder[] = ".storage/no-climate-XXXXXX";
    ASSERT_NE(nullptr, mkdtemp(folder));
    this->storage_.path = folder;
    // Outlives the case: the switch keeps a release callback into it.
    this->engine_ = new TestEngine();
    this->engine_->set_storage(&this->storage_);
    this->engine_->setup();
  }
  void TearDown() override {
    this->engine_->forget();
    const std::string folder = this->storage_.path + "/schedules";
    DIR *dir = opendir(folder.c_str());
    while (dir != nullptr) {
      struct dirent *entry = readdir(dir);
      if (entry == nullptr)
        break;
      ::remove((folder + "/" + entry->d_name).c_str());
    }
    if (dir != nullptr)
      closedir(dir);
    rmdir(folder.c_str());
    rmdir(this->storage_.path.c_str());
  }
  static ScheduleConfig on_monday_morning(TargetType type, const char *target) {
    ScheduleConfig config;
    config.name = "Morning";
    config.target_type = type;
    config.target_id = target;
    config.add_point(8 * 60, type == TargetType::RELAY ? ValueType::ON : ValueType::OFF);
    return config;
  }

  FakeStorage storage_;
  TestEngine *engine_{nullptr};
};

}  // namespace

TEST_F(NoClimate, ARelayScheduleRuns) {
  Result created = this->engine_->create(on_monday_morning(TargetType::RELAY, "relay_1"));
  ASSERT_TRUE(created.ok) << created.error;
  EXPECT_EQ("", created.warning);
  this->engine_->local = monday_nine();
  this->engine_->tick();
  EXPECT_TRUE(relay1().state);
  EXPECT_EQ(ScheduleState::RUNNING, this->engine_->status("morning").state);
  EXPECT_EQ("Relay 1", this->engine_->target_name(*this->engine_->get("morning")));
}

TEST_F(NoClimate, AThermostatTargetHasNothingToDrive) {
  Result created = this->engine_->create(on_monday_morning(TargetType::THERMOSTAT, "living-room"));
  ASSERT_TRUE(created.ok) << created.error;
  EXPECT_EQ("not running: this device has no thermostats", created.warning);
  this->engine_->local = monday_nine();
  this->engine_->tick();
  const ScheduleStatus status = this->engine_->status("morning");
  EXPECT_EQ(ScheduleState::MISSING_TARGET, status.state);
  EXPECT_EQ("This device has no thermostats", status.reason);
  EXPECT_EQ("living-room", this->engine_->target_name(*this->engine_->get("morning")));
}

TEST_F(NoClimate, NoThermostatIsOffered) {
  EXPECT_TRUE(this->engine_->thermostat_targets().empty());
  const std::vector<RelayTarget> relays = this->engine_->relay_targets();
  ASSERT_EQ(1u, relays.size());
  EXPECT_EQ("relay_1", relays[0].object_id);
}

// --- over HTTP ---

namespace {

class NoClimateEditor : public NoClimate {
 protected:
  void SetUp() override {
    NoClimate::SetUp();
    this->editor_ = std::make_unique<web_schedule_editor::WebScheduleEditor>(&this->base_, this->engine_);
    this->editor_->setup();
  }
  // @p route below the API, with its query; the answer's status into @p code.
  JsonDocument call(http_method method, const std::string &route, const std::string &body, int *code) {
    AsyncWebServerRequest request(method, "/schedule-editor/api/" + route, body, "application/json");
    EXPECT_TRUE(this->base_.get_server()->dispatch(request)) << route;
    *code = request.response_code;
    JsonDocument doc;
    EXPECT_EQ(deserializeJson(doc, request.response_body), DeserializationError::Ok) << request.response_body;
    return doc;
  }

  web_server_base::WebServerBase base_;
  std::unique_ptr<web_schedule_editor::WebScheduleEditor> editor_;
};

}  // namespace

TEST_F(NoClimateEditor, TargetsOffersTheRelaysAndNoThermostat) {
  int code = 0;
  JsonDocument targets = this->call(HTTP_GET, "targets", "", &code);
  EXPECT_EQ(200, code);
  ASSERT_TRUE(targets["thermostats"].is<JsonArray>());
  EXPECT_EQ(0u, targets["thermostats"].size());
  ASSERT_EQ(1u, targets["relays"].size());
  EXPECT_EQ("relay_1", targets["relays"][0]["object_id"].as<std::string>());
}

TEST_F(NoClimateEditor, AThermostatScheduleIsSavedAndSaysWhyItDoesNotRun) {
  int code = 0;
  JsonDocument saved = this->call(HTTP_POST, "save",
                                  R"({"name":"Heat","target":{"type":"thermostat","id":"living-room"},)"
                                  R"("week":{"mon":[{"at":"07:00","value":{"type":"target","target":21}}]}})",
                                  &code);
  EXPECT_EQ(200, code);
  EXPECT_EQ("Schedule created; not running: this device has no thermostats", saved["message"].as<std::string>());
  EXPECT_EQ("not running: this device has no thermostats", saved["warning"].as<std::string>());
  this->engine_->local = monday_nine();
  this->engine_->tick();
  JsonDocument list = this->call(HTTP_GET, "list", "", &code);
  EXPECT_EQ(200, code);
  JsonObject row = list["schedules"][0];
  EXPECT_EQ("missing_target", row["status"]["state"].as<std::string>());
  EXPECT_EQ("This device has no thermostats", row["status"]["reason"].as<std::string>());
  EXPECT_EQ("living-room", row["target"]["name"].as<std::string>());
}

TEST_F(NoClimateEditor, ARelayScheduleRunsOverHttp) {
  this->engine_->local = monday_nine();
  this->engine_->tick();
  int code = 0;
  JsonDocument saved = this->call(HTTP_POST, "save",
                                  R"({"name":"Morning","target":{"type":"relay","id":"relay_1"},)"
                                  R"("week":{"mon":[{"at":"08:00","value":{"type":"on"}}]}})",
                                  &code);
  EXPECT_EQ(200, code);
  EXPECT_EQ("Schedule created", saved["message"].as<std::string>());
  EXPECT_TRUE(relay1().state);
}

}  // namespace esphome::schedules::testing
