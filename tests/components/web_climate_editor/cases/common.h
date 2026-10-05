#pragma once
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "esphome/components/climate/climate.h"
#include "esphome/components/climate_hub/climate_hub.h"
#include "esphome/components/dir_storage/dir_storage.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/web_climate_editor/web_climate_editor.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

namespace esphome::web_climate_editor::testing {

// What the component logged. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> lines;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() { this->lines.clear(); }
  bool has(const char *needle) const {
    return std::any_of(this->lines.begin(), this->lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t, const char *, const char *message, size_t len) {
    static_cast<LogCapture *>(self)->lines.emplace_back(message, len);
  }
};

class FakeSwitch : public switch_::Switch {
 protected:
  void write_state(bool state) override { this->publish_state(state); }
};

// A climate the YAML declared: no thermostat may take its name.
class YamlClimate : public climate::Climate {
 protected:
  climate::ClimateTraits traits() override {
    climate::ClimateTraits traits;
    traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
    return traits;
  }
  void control(const climate::ClimateCall &) override {}
};

// The entities a thermostat may name. Registered once: App keeps the pointers for the life of
// the process.
struct Entities {
  sensor::Sensor room;
  sensor::Sensor floor;
  sensor::Sensor probe;
  // Visible, but not in °C: no thermostat input.
  sensor::Sensor uptime;
  sensor::Sensor counter;
  FakeSwitch relay1;
  FakeSwitch relay2;
  FakeSwitch hidden;
  YamlClimate hall;
};

// The entity field that gives `unit`, from the table codegen built out of test.yaml's units;
// 0, no unit, when test.yaml declares none in it.
inline uint32_t unit_field(const char *unit) {
  for (uint32_t index = 1; index <= 0xFF; index++) {
    const char *known = entity_uom_lookup(static_cast<uint8_t>(index));
    if (*known == '\0')
      break;
    if (strcmp(known, unit) == 0)
      return index << ENTITY_FIELD_UOM_SHIFT;
  }
  return 0;
}

inline Entities &entities() {
  static Entities *instance = [] {
    auto *e = new Entities();
    const uint32_t celsius = unit_field("°C");
    App.register_sensor(&e->room, "Room", fnv1_hash("room"), celsius);
    App.register_sensor(&e->floor, "Floor", fnv1_hash("floor"), celsius);
    App.register_sensor(&e->probe, "Probe", fnv1_hash("probe"), 1u << ENTITY_FIELD_INTERNAL_SHIFT);
    App.register_sensor(&e->uptime, "Uptime", fnv1_hash("uptime"), unit_field("s"));
    App.register_sensor(&e->counter, "Counter", fnv1_hash("counter"), 0);
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    App.register_switch(&e->relay2, "Relay 2", fnv1_hash("relay_2"), 0);
    App.register_switch(&e->hidden, "Hidden", fnv1_hash("hidden"), 1u << ENTITY_FIELD_INTERNAL_SHIFT);
    App.register_climate(&e->hall, "Hall", fnv1_hash("hall"), 0);
    return e;
  }();
  return *instance;
}

inline dir_storage::DirStorage &storage() {
  static auto *instance = new dir_storage::DirStorage();
  return *instance;
}

// The hub with its seams taken. A host build has one task, so every job runs inline and a
// handler that read the documents where it stands would answer exactly like one that handed
// the read over: what crossed is counted here instead, and `loop_busy` refuses a job the way
// the dispatcher does when the loop task never gets to it. The crossing itself is
// tests/components/loop_job/. `refuse_remove` makes the partition refuse an unlink.
class TestHub : public climate_hub::ClimateHub {
 public:
  uint32_t ms{100000};
  int jobs{0};
  bool loop_busy{false};
  int resyncs{0};
  bool refuse_remove{false};
  size_t max_file_bytes{climate_hub::CONFIG_MAX_BYTES};

  uint32_t now_ms() const override { return this->ms; }
  bool run_on_loop(std::function<bool()> &&job) override {
    this->jobs++;
    if (this->loop_busy)
      return false;
    return climate_hub::ClimateHub::run_on_loop(std::move(job));
  }

  // Every climate entity in use, as when App had no room for the whole pool.
  void take_every_slot() { this->free_.clear(); }

  // Back to a hub that has loaded nothing, its pool as setup() left it. App keeps the entities
  // and the sensors keep their callbacks into the hub, so it is reset, never replaced.
  void reset() {
    this->reset_to_construction_state();
    for (Slot *slot : this->slots_)
      this->stop_(slot);
    this->free_.assign(this->slots_.begin(), this->slots_.end());
    this->claims_.clear();
    this->relay_history_.clear();
    for (auto &sub : this->sensor_subs_)
      sub->last = climate_hub::Reading{};
    this->store_.clear();
    this->dirty_.clear();
    this->waiting_.clear();
    this->cancel_timeout("ha_resync");
    this->ms = 100000;
    this->jobs = 0;
    this->loop_busy = false;
    this->resyncs = 0;
    this->refuse_remove = false;
    this->max_file_bytes = climate_hub::CONFIG_MAX_BYTES;
  }

 protected:
  void resync_home_assistant_() override { this->resyncs++; }
  bool remove_file_(const std::string &path) override {
    return !this->refuse_remove && climate_hub::ClimateHub::remove_file_(path);
  }
  climate_hub::EncodeError encode_(const climate_hub::ClimateConfig &config, std::string *json) const override {
    return config.encode(json, this->max_file_bytes);
  }
};

// One hub for the whole process: its pool registers with App in the first setup(), and App has
// room for exactly one pool.
inline TestHub &hub() {
  static TestHub *instance = [] {
    auto *h = new TestHub();
    h->set_storage(&storage());
    h->set_folder_path("climates");
    h->set_max_controllers(3);
    return h;
  }();
  return *instance;
}

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
  std::string header(const char *name) const {
    for (const auto &header : this->headers) {
      if (header.first == name)
        return header.second;
    }
    return "";
  }
};

static const char *const LIVING_ROOM =
    R"({"name":"Living Room","kind":"pid","sensor_id":"room","heat":{"relay_id":"relay_1"},"mode":"heat","setpoint":22})";
static const char *const FLOOR =
    R"({"name":"Floor","kind":"bang_bang","sensor_id":"floor","heat":{"relay_id":"relay_2"},"mode":"heat","setpoint":24})";

// @p json with @p fields put first, e.g. with(LIVING_ROOM, R"("enabled":false)").
inline std::string with(const char *json, const std::string &fields) {
  std::string out = json;
  out.insert(1, fields + ",");
  return out;
}

// The editor over the process's hub over a temporary directory, reached through the harness's
// web_server_base stand-in exactly as a request from the network would reach it.
class Editor : public ::testing::Test {
 protected:
  void SetUp() override {
    entities();
    for (sensor::Sensor *sensor : {&entities().room, &entities().floor}) {
      sensor->state = NAN;
      sensor->set_has_state(false);
    }
    for (FakeSwitch *relay : {&entities().relay1, &entities().relay2})
      relay->publish_state(false);
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->base_path = folder;
    storage().set_base_path(this->base_path);
    storage().setup();
    hub().reset();
    hub().setup();
    ASSERT_FALSE(hub().is_failed());
    this->editor = std::make_unique<WebClimateEditor>(&this->base, &hub());
    this->editor->setup();
  }

  void TearDown() override {
    hub().reset();
    storage().set_base_path(this->base_path);
    for (const std::string &name : this->files())
      remove((this->folder() + "/" + name).c_str());
    rmdir(this->folder().c_str());
    rmdir(this->base_path.c_str());
  }

  std::string folder() const { return this->base_path + "/climates"; }

  std::vector<std::string> files() const {
    std::vector<std::string> names;
    DIR *dir = opendir(this->folder().c_str());
    if (dir == nullptr)
      return names;
    while (struct dirent *entry = readdir(dir)) {
      if (entry->d_name[0] != '.')
        names.emplace_back(entry->d_name);
    }
    closedir(dir);
    return names;
  }

  // What @p name in the folder holds, "" when it is not there.
  std::string file(const std::string &name) const {
    std::ifstream in(this->folder() + "/" + name);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  // @p target is the path with its query; @p origin is what a page on another site would
  // carry, nullptr a client that sends none.
  Reply request(http_method method, const std::string &target, const std::string &body = "",
                const std::string &content_type = "application/json", const char *origin = nullptr) {
    AsyncWebServerRequest request(method, target, body, content_type);
    request.set_header("Host", HOST);
    if (origin != nullptr)
      request.set_header("Origin", origin);
    Reply reply;
    reply.claimed = this->base.get_server()->dispatch(request);
    reply.code = request.response_code;
    reply.type = request.response_type;
    reply.body = request.response_body;
    reply.headers = request.response_headers;
    EXPECT_LE(request.responses, 1) << target << " was answered twice";
    if (!reply.body.empty() && reply.type == "application/json")
      EXPECT_EQ(deserializeJson(reply.json, reply.body), DeserializationError::Ok) << reply.body;
    return reply;
  }
  Reply call(http_method method, const std::string &route, const std::string &body = "",
             const std::string &content_type = "application/json", const char *origin = nullptr) {
    return this->request(method, "/climate-editor/api/" + route, body, content_type, origin);
  }
  Reply get(const std::string &route) { return this->call(HTTP_GET, route); }
  Reply post(const std::string &route, const std::string &body = "") { return this->call(HTTP_POST, route, body); }

  // Creates a thermostat and returns its id, "" when the API refused it.
  std::string create(const char *json) {
    Reply reply = this->post("save", json);
    EXPECT_EQ(reply.code, 200) << reply.body;
    return reply["id"] | "";
  }

  // The Host every case is addressed to; a page on another site says so by carrying an Origin
  // that is not this.
  static constexpr const char *HOST = "device.local";

  std::string base_path;
  web_server_base::WebServerBase base;
  std::unique_ptr<WebClimateEditor> editor;
};

}  // namespace esphome::web_climate_editor::testing
