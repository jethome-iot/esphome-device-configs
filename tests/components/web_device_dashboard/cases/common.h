#pragma once
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/config_json/config_json.h"
#include "esphome/components/config_json/settings_base_json.h"
#include "esphome/components/dir_storage/dir_storage.h"
#include "esphome/components/host/preferences.h"
#include "esphome/components/jethome_board_info/jethome_board_info.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/host/preferences.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/web_auth/web_auth.h"
#include "esphome/components/web_device_dashboard/web_device_dashboard.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

namespace esphome::web_device_dashboard::testing {

// The preferences a factory reset clears. The generated setup would have installed the host
// backend before anything asked for a preference; main.cpp never runs that setup. The prefs
// directory goes next to the test binary rather than into $HOME.
inline void install_preferences() {
  [[maybe_unused]] static const bool ONCE = [] {
    setenv("ESPHOME_PREFDIR", ".prefs", 0);
    host::setup_preferences();
    return true;
  }();
}

// What check_method_ logs: off ESP32 the Allow value never reaches a header, so the warning is
// the only place a test can read it. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() {
    this->errors.clear();
    this->warnings.clear();
  }
  bool has_warning(const char *needle) const { return has(this->warnings, needle); }
  bool has_error(const char *needle) const { return has(this->errors, needle); }

 protected:
  static bool has(const std::vector<std::string> &lines, const char *needle) {
    return std::any_of(lines.begin(), lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    auto *capture = static_cast<LogCapture *>(self);
    if (level == ESPHOME_LOG_LEVEL_ERROR) {
      capture->errors.emplace_back(message, len);
    } else if (level == ESPHOME_LOG_LEVEL_WARN) {
      capture->warnings.emplace_back(message, len);
    }
  }
};

class FakeSwitch : public switch_::Switch {
 protected:
  void write_state(bool state) override { this->publish_state(state); }
};

// The entities /api/device/entities may list, and the name /api/device/info and /network
// report. Registered once: App keeps the pointers for the life of the process.
struct Entities {
  binary_sensor::BinarySensor in1;
  FakeSwitch relay1;
  FakeSwitch hidden;
};

inline Entities &entities() {
  static Entities *instance = [] {
    // The generated setup would have named the device before anything asked App for it.
    App.pre_setup("dashboard-test", 14, "Dashboard Test", 14);
    auto *e = new Entities();
    App.register_binary_sensor(&e->in1, "In 1", fnv1_hash("in_1"), 0);
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    App.register_switch(&e->hidden, "Hidden", fnv1_hash("hidden"), 1u << ENTITY_FIELD_INTERNAL_SHIFT);
    return e;
  }();
  return *instance;
}

// One record per entity: the smallest shape the POST route's hooks can carry.
struct TestRecord {
  std::string source_name_;
  bool inverted{false};

  const char *source_name() const { return this->source_name_.c_str(); }

  void to_json(JsonObject obj, uint32_t) const {
    obj["source_name"] = this->source_name_;
    obj["inverted"] = this->inverted;
  }
  bool from_json(JsonObject obj, uint32_t) {
    if (!obj["source_name"].is<const char *>())
      return false;
    this->source_name_ = obj["source_name"].as<const char *>();
    this->inverted = obj["inverted"] | false;
    return true;
  }
};

// A settings type under one of the keys the entity index looks for, with the record hooks the
// dashboard drives. The key is a constructor argument so one class covers both entity kinds.
class TestSettings : public config_json::SettingsBaseJsonTyped<TestSettings, TestRecord> {
  friend class config_json::SettingsBaseJsonTyped<TestSettings, TestRecord>;

 public:
  static constexpr const char *TAG = "test_settings";
  static constexpr float APPLY_PRIORITY = setup_priority::HARDWARE + 1.0f;

  explicit TestSettings(const char *key) : key_(key) {}
  const char *get_key() override { return this->key_; }

  // What apply_record() handed over, in order.
  std::vector<std::string> applied;

  TestRecord *update_record(JsonObject obj) {
    const char *name = obj["source_name"];
    if (name == nullptr || name[0] == '\0')
      return nullptr;
    TestRecord *record = this->find(name);
    if (record == nullptr) {
      record = new TestRecord();  // NOLINT(cppcoreguidelines-owning-memory)
      record->source_name_ = name;
      this->records_.push_back(record);
    }
    record->inverted = obj["settings"]["inverted"] | false;
    this->mark_dirty();
    return record;
  }

  void *can_delete(JsonObject obj) override {
    const char *name = obj["source_name"];
    return name == nullptr ? nullptr : this->find(name);
  }

  bool delete_record(void *record) override {
    auto it = std::find(this->records_.begin(), this->records_.end(), static_cast<TestRecord *>(record));
    if (it == this->records_.end())
      return false;
    delete *it;  // NOLINT(cppcoreguidelines-owning-memory)
    this->records_.erase(it);
    this->mark_dirty();
    return true;
  }

  void write_settings_meta(JsonObject obj) override {
    JsonArray fields = obj["fields"].to<JsonArray>();
    fields.add("inverted");
  }

  // A record the API did not put there, for the reads that have to find more than one.
  void seed(const char *name, bool inverted) {
    auto *record = new TestRecord();  // NOLINT(cppcoreguidelines-owning-memory)
    record->source_name_ = name;
    record->inverted = inverted;
    this->records_.push_back(record);
  }

  TestRecord *find(const char *name) {
    for (auto *record : this->records_) {
      if (record->source_name_ == name)
        return record;
    }
    return nullptr;
  }

  // "name=inverted ..." for a compact comparison.
  std::string report() const {
    std::string out;
    for (const auto *record : this->records_) {
      if (!out.empty())
        out += " ";
      out += record->source_name_ + "=" + (record->inverted ? "1" : "0");
    }
    return out;
  }

  void forget() {
    this->clear_records_();
    this->applied.clear();
    this->clear_dirty();
  }

 protected:
  void apply_record_(TestRecord *record) { this->applied.push_back(record->source_name_); }

  const char *key_;
};

// The keeper and its settings outlive every test: a save hands App's scheduler an entry that
// points at the keeper, and nothing here runs the scheduler to the end of that timeout.
struct Store {
  config_json::ConfigJsonKeeper keeper;
  TestSettings sw{"switch"};
  TestSettings bs{"binary_sensor"};
  // A type the entity index has no branch for: only switch and binary_sensor are listed.
  TestSettings other{"test"};
};

inline Store &store() {
  static Store *instance = [] {
    auto *s = new Store();
    s->keeper.add_settings(&s->sw);
    s->keeper.add_settings(&s->bs);
    s->keeper.add_settings(&s->other);
    return s;
  }();
  return *instance;
}

// A board with its fields already in place: reading them off an EEPROM is what the
// jethome_board_info suite covers, so the dashboard's tests set them directly.
class FakeBoard : public jethome_board_info::JetHomeBoardInfo {
 public:
  static constexpr uint8_t MAC[6] = {0x00, 0x1E, 0x06, 0xAA, 0xBB, 0xCC};

  void read_header() {
    this->valid_ = true;
    this->crc_valid_ = true;
    this->data_.version = 4;
    this->data_.signature_version = jethome_board_info::SIG_NONE;
    this->data_.timestamp = 1700000000;
    memcpy(this->data_.mac, MAC, sizeof(MAC));
    this->boardname_ = "JetHub D1";
    this->boardversion_ = "4";
    this->board_serial_ = "B0001";
    this->usid_ = "JHD10014202301AA0000000042";
    this->cpuid_ = "0123456789abcdef";
  }

  void read_device_identity() {
    this->devid_valid_ = true;
    this->device_model_ = "JetHub D1+";
    this->device_serial_ = "0000000042";
    this->hw_revision_ = "1.2";
  }

  void sign(uint8_t version) {
    this->devid_.signature_version = version;
    this->data_.signature_version = version;
    memset(this->devid_.signature, 0xAB, jethome_board_info::SIGNATURE_SIZE);
    memset(this->data_.signature, 0xAB, jethome_board_info::SIGNATURE_SIZE);
  }

  void check_serial(bool matches) {
    this->serial_checked_ = true;
    this->serial_match_ = matches;
  }
};

// One answered request: whether a handler claimed it, and what it said.
struct Reply {
  bool claimed{false};
  int code{0};
  std::string type;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  JsonDocument json;

  JsonVariant operator[](const char *key) { return this->json[key]; }
  std::string error() { return this->json["error"] | ""; }
  std::string message() { return this->json["message"] | ""; }
  bool success() { return this->json["success"] | false; }
  std::string header(const char *name) const {
    for (const auto &entry : this->headers) {
      if (entry.first == name)
        return entry.second;
    }
    return "";
  }
};

static const char *const UPDATE_RELAY_1 = R"({"type":"switch","source_name":"relay_1","settings":{"inverted":true}})";
static const char *const DELETE_RELAY_1 = R"({"type":"switch","source_name":"relay_1","action":"delete"})";

// The dashboard with its request-scoped body state in reach: the buffer is protected, and
// handleRequest releases it before it returns. The system actions are held back too --
// App.safe_reboot() would end the process, and a host build has no second app slot.
class TestDashboard : public WebDeviceDashboard {
 public:
  using WebDeviceDashboard::route_for_;
  using WebDeviceDashboard::WebDeviceDashboard;

  const std::string &body() const { return this->body_; }
  size_t body_capacity() const { return this->body_.capacity(); }
  size_t body_total() const { return this->body_total_; }
  size_t body_received() const { return this->body_received_; }
  bool body_too_large() const { return this->body_too_large_; }

  // The deferred half of a system action runs in the loop call right after the request
  // instead of half a second later, so a test can see it happen.
  void act_without_waiting() { this->action_delay_ms_ = 0; }

  int restarts{0};
  int rollbacks{0};
  // What rollback_target_() answers while stub_rollback is set; without it the build's own
  // answer stands, which off ESP32 is "nothing to roll back to".
  bool stub_rollback{false};
  RollbackTarget rollback;
  const char *rollback_error{nullptr};

 protected:
  void restart_() override { this->restarts++; }
  RollbackTarget rollback_target_() const override {
    return this->stub_rollback ? this->rollback : WebDeviceDashboard::rollback_target_();
  }
  const char *select_rollback_(const RollbackTarget & /*target*/) override {
    this->rollbacks++;
    return this->rollback_error;
  }
};

// The dashboard behind the harness's web_server_base stand-in, reached exactly as a request
// off the socket would reach it.
class Dashboard : public ::testing::Test {
 protected:
  void SetUp() override {
    entities();
    install_preferences();
    config_json::global_config_json_keeper = &store().keeper;
    store().sw.forget();
    store().bs.forget();
    store().other.forget();
    // A device that has never had its credentials changed; the auth cases store their own.
    global_preferences->sync();
    global_preferences->reset();
    global_preferences->sync();
    this->auth = std::make_unique<web_auth::WebAuth>(&this->base);
    this->auth->set_default_credentials("admin", "hunter2");
    this->auth->set_preference_hash(fnv1_hash("test_auth"));
    this->auth->setup();

    this->storage.set_base_path(".storage");
    this->storage.setup();
    this->dashboard = std::make_unique<TestDashboard>(&this->base);
    this->dashboard->set_board_info(&this->board);
    this->dashboard->set_storage(&this->storage);
    this->dashboard->act_without_waiting();
    this->dashboard->setup();
    LogCapture::instance().clear();
  }

  void TearDown() override {
    // Runs the apply the POST route deferred, before the dashboard it points at goes.
    App.scheduler.call(millis());
    config_json::global_config_json_keeper = &store().keeper;
    store().keeper.cancel_pending_save();
  }

  // What the loop task does between two requests: run what handleRequest deferred.
  static void loop() { App.scheduler.call(millis()); }

  // The Host every case is addressed to; a page on another site says so by carrying an Origin
  // that is not this.
  static constexpr const char *HOST = "device.local";

  // One request through the server, the way web_server_idf delivers it: a raw body in @p chunk
  // sized pieces through handleBody, then handleRequest. A form body never reaches handleBody.
  // @p origin is what a page on another site would carry; nullptr is a client that sends none.
  Reply call(http_method method, const std::string &url, const std::string &body = "", size_t chunk = 512,
             const std::string &content_type = "application/json", const char *origin = nullptr) {
    AsyncWebServerRequest request(method, url, body, content_type);
    request.set_header("Host", HOST);
    if (origin != nullptr)
      request.set_header("Origin", origin);
    Reply reply;
    reply.claimed = this->base.get_server()->dispatch(request, chunk);
    reply.code = request.response_code;
    reply.type = request.response_type;
    reply.body = request.response_body;
    reply.headers = request.response_headers;
    EXPECT_LE(request.responses, 1) << url << " was answered twice";
    if (!reply.body.empty() && reply.type == "application/json")
      EXPECT_EQ(deserializeJson(reply.json, reply.body), DeserializationError::Ok) << reply.body;
    return reply;
  }
  Reply get(const std::string &url) { return this->call(HTTP_GET, url); }
  Reply post(const std::string &url, const std::string &body = "", size_t chunk = 512) {
    return this->call(HTTP_POST, url, body, chunk);
  }

  // What the system routes take, with the token this same API publishes: the last three
  // octets of the MAC /api/device/info reports.
  std::string confirmation(const char *token = nullptr) {
    const std::string mac = this->get("/api/device/info")["base_mac_address"].as<std::string>();
    const std::string value = token != nullptr ? token : mac.substr(mac.size() - 8);
    return R"({"confirm":true,"confirm_token":")" + value + R"("})";
  }

  bool claims(const std::string &url) {
    AsyncWebServerRequest request(HTTP_GET, url);
    return this->dashboard->canHandle(&request);
  }

  // The body of a request that never reached handleRequest: @p delivered bytes of @p body,
  // announced as the whole of it.
  void feed(const std::string &body, size_t chunk, size_t delivered) {
    AsyncWebServerRequest request(HTTP_POST, "/api/device/entity-settings", body);
    std::string copy = body;
    for (size_t index = 0; index < delivered; index += chunk) {
      const size_t len = std::min(chunk, delivered - index);
      this->dashboard->handleBody(&request, reinterpret_cast<uint8_t *>(&copy[index]), len, index, body.size());
    }
  }
  void feed(const std::string &body, size_t chunk) { this->feed(body, chunk, body.size()); }

  web_server_base::WebServerBase base;
  FakeBoard board;
  std::unique_ptr<web_auth::WebAuth> auth;
  dir_storage::DirStorage storage;
  std::unique_ptr<TestDashboard> dashboard;
};

}  // namespace esphome::web_device_dashboard::testing
