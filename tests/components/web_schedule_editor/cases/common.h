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
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "esphome/components/climate_hub/climate_hub.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/schedules/schedule_engine.h"
#include "esphome/components/schedules/week_clock.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/switch_hold/switch_hold.h"
#include "esphome/components/web_schedule_editor/web_schedule_editor.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

namespace esphome::web_schedule_editor::testing {

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

// Remembers every write; the state follows it like an optimistic template switch.
class FakeSwitch : public switch_::Switch {
 public:
  std::vector<bool> writes;

 protected:
  void write_state(bool state) override {
    this->writes.push_back(state);
    this->publish_state(state);
  }
};

// A mounted directory, the way littlefs_storage presents the partition.
class FakeStorage : public filesystem_storage_abstract::FilesystemStorageAbstract {
 public:
  std::string path;
  bool is_mounted() const override { return true; }
  const std::string &get_base_path() const override { return this->path; }
  const char *get_filesystem_type() const override { return "Directory"; }
  bool request_format() override { return false; }
};

// The entities a schedule may name. Registered once: App keeps the pointers for the life of
// the process. Relays 1 to 3 are contract.json's; the thermostats run on the boiler and the pump.
struct Entities {
  sensor::Sensor room;
  FakeSwitch relay1;
  FakeSwitch relay2;
  FakeSwitch relay3;
  FakeSwitch boiler;
  FakeSwitch pump;
  FakeSwitch hidden;
};

// The entity field that gives `unit`, from the table codegen built out of test.yaml's units.
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
    App.register_sensor(&e->room, "Room", fnv1_hash("room"), unit_field("°C"));
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    App.register_switch(&e->relay2, "Relay 2", fnv1_hash("relay_2"), 0);
    App.register_switch(&e->relay3, "Relay 3", fnv1_hash("relay_3"), 0);
    App.register_switch(&e->boiler, "Boiler", fnv1_hash("boiler"), 0);
    App.register_switch(&e->pump, "Pump", fnv1_hash("pump"), 0);
    App.register_switch(&e->hidden, "Hidden", fnv1_hash("hidden"), 1u << ENTITY_FIELD_INTERNAL_SHIFT);
    return e;
  }();
  return *instance;
}

inline void reset_entities() {
  Entities &e = entities();
  for (FakeSwitch *sw : {&e.relay1, &e.relay2, &e.relay3, &e.boiler, &e.pump, &e.hidden}) {
    sw->publish_state(false);
    sw->writes.clear();
  }
}

inline std::vector<std::string> list_dir(const std::string &path) {
  std::vector<std::string> names;
  DIR *dir = opendir(path.c_str());
  if (dir == nullptr)
    return names;
  while (struct dirent *entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name != "." && name != "..")
      names.push_back(name);
  }
  closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

// Empties `path`, the folders in it included, and removes it.
inline void remove_tree(const std::string &path) {
  for (const std::string &name : list_dir(path)) {
    const std::string inner = path + "/" + name;
    struct stat st;
    if (stat(inner.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      chmod(inner.c_str(), 0755);
      remove_tree(inner);
    } else {
      ::remove(inner.c_str());
    }
  }
  rmdir(path.c_str());
}

// The one hub of the process: App has room for the one pool test.yaml declares. Over a folder
// of its own, emptied first so a crashed run leaves nothing behind.
inline climate_hub::ClimateHub &hub() {
  static climate_hub::ClimateHub *instance = [] {
    entities();
    static FakeStorage storage;
    mkdir(".storage", 0755);
    remove_tree(".storage/hub");
    mkdir(".storage/hub", 0755);
    storage.path = ".storage/hub";
    auto *h = new climate_hub::ClimateHub();
    h->set_storage(&storage);
    h->set_max_controllers(3);
    h->setup();
    return h;
  }();
  return *instance;
}

inline climate_hub::PresetConfig preset(const char *name, float setpoint) {
  climate_hub::PresetConfig p;
  p.name = name;
  p.setpoint = setpoint;
  return p;
}

// contract.json's thermostats: Living Room runs and heats, with comfort and eco; Hall is
// switched off and has no presets. Each case starts with them as they are here.
inline void reset_thermostats() {
  std::vector<std::string> ids;
  for (const auto &config : hub().store().all())
    ids.push_back(config->id);
  for (const std::string &id : ids)
    hub().remove(id);
  climate_hub::ClimateConfig living;
  living.name = "Living Room";
  living.sensor_id = "room";
  living.heat.relay_id = "boiler";
  living.mode = climate_hub::HubMode::HEAT;
  living.setpoint = 21.f;
  living.visual.min_temperature = 5.f;
  living.visual.max_temperature = 30.f;
  living.visual.step = 0.5f;
  living.presets = {preset("Comfort", 22.f), preset("Eco", 19.f)};
  climate_hub::Result made = hub().create(living);
  ASSERT_TRUE(made.ok) << made.error;
  ASSERT_EQ(made.id, "living-room");
  climate_hub::ClimateConfig hall;
  hall.name = "Hall";
  hall.enabled = false;
  hall.sensor_id = "room";
  hall.heat.relay_id = "pump";
  hall.mode = climate_hub::HubMode::OFF;
  hall.visual.min_temperature = 5.f;
  hall.visual.max_temperature = 30.f;
  hall.visual.step = 0.5f;
  made = hub().create(hall);
  ASSERT_TRUE(made.ok) << made.error;
  ASSERT_EQ(made.id, "hall");
}

// Holds relays under thermostats' names, in place of the hub, which holds only the relays of
// the thermostats it runs.
class FakeHolder : public switch_hold::SwitchHolder {
 public:
  std::vector<std::pair<const switch_::Switch *, std::string>> held;
  std::string holder_of(const switch_::Switch *sw) const override {
    for (const auto &[relay, name] : this->held) {
      if (relay == sw)
        return name;
    }
    return "";
  }
};

inline FakeHolder &fake_holder() {
  static auto *instance = new FakeHolder();
  return *instance;
}

// Local seconds of `day` (0 is Monday) at `hour`:`minute`:`second`, in the week of 2026-10-26.
inline int64_t at_local(int day, int hour, int minute, int second = 0) {
  return (schedules::days_from_civil(2026, 10, 26) + day) * 86400 + hour * 3600 + minute * 60 + second;
}

// contract.json's `now`: Wednesday, 10:15:42.
inline int64_t wednesday() { return at_local(2, 10, 15, 42); }

// The engine with its seams taken. A host build has one task, so every job runs inline and a
// handler that read the schedules where it stands would answer exactly like one that handed
// the read over: what crossed is counted here instead, and `loop_busy` refuses a job the way
// the dispatcher does when the loop task never gets to it. The local clock is `local`, 0 for
// none valid yet; what waits for the loop waits in `later` until run_later().
class TestEngine : public schedules::ScheduleEngine {
 public:
  int64_t local{0};
  uint64_t mono{5000000};
  int jobs{0};
  bool loop_busy{false};
  bool refuse_remove{false};
  size_t max_file_bytes{schedules::MAX_FILE_BYTES};
  // The heap's room as has_heap_room() reads it: free and the largest block alike.
  size_t heap_free{SIZE_MAX};
  std::vector<std::function<void()>> later;

  bool run_on_loop(std::function<bool()> &&job) override {
    this->jobs++;
    if (this->loop_busy)
      return false;
    return schedules::ScheduleEngine::run_on_loop(std::move(job));
  }

  void tick() { this->tick_(); }
  size_t run_later() {
    size_t ran = 0;
    while (!this->later.empty()) {
      auto now = std::move(this->later);
      this->later.clear();
      for (auto &job : now) {
        job();
        ran++;
      }
    }
    return ran;
  }

  // A boot over the folder as it is now, the clock not valid yet. The first is setup() itself;
  // later ones skip what it registered with the hub and the relays, which keep it.
  void boot() {
    this->entries_.clear();
    this->later.clear();
    this->clock_valid_ = false;
    this->mark_ = ClockMark{};
    this->reset_to_construction_state();
    if (!this->set_up_) {
      this->set_up_ = true;
      this->setup();
      return;
    }
    if (!this->load_())
      this->mark_failed();
  }
  // Drives nothing until the next boot.
  void forget() {
    this->entries_.clear();
    this->later.clear();
  }
  void reset_seams() {
    this->local = 0;
    this->jobs = 0;
    this->loop_busy = false;
    this->refuse_remove = false;
    this->max_file_bytes = schedules::MAX_FILE_BYTES;
    this->heap_free = SIZE_MAX;
  }

 protected:
  ESPTime clock_now_() override { return this->local == 0 ? ESPTime{} : ESPTime::from_epoch_utc(this->local); }
  uint64_t mono_ms_() override { return this->mono; }
  void later_(std::function<void()> &&job) override { this->later.push_back(std::move(job)); }
  size_t max_file_bytes_() const override { return this->max_file_bytes; }
  schedules::HeapRoom heap_() const override {
    schedules::HeapRoom heap;
    heap.free = this->heap_free;
    heap.largest_block = this->heap_free;
    return heap;
  }
  bool remove_file_(const std::string &path) override {
    return !this->refuse_remove && schedules::ScheduleEngine::remove_file_(path);
  }

  bool set_up_{false};
};

inline FakeStorage &storage() {
  static auto *instance = new FakeStorage();
  return *instance;
}

// One engine for the whole process: the hub and the relays keep a callback into it.
inline TestEngine &engine() {
  static TestEngine *instance = [] {
    hub();
    auto *e = new TestEngine();
    e->set_storage(&storage());
    e->set_folder_path("schedules");
    e->set_max_schedules(3);
    return e;
  }();
  return *instance;
}

// The editor with its heap seam taken: `allocator`, when set, is what the body, a parsed body
// and the answers draw on.
class TestEditor : public WebScheduleEditor {
 public:
  using WebScheduleEditor::WebScheduleEditor;
  ArduinoJson::Allocator *allocator{nullptr};

 protected:
  ArduinoJson::Allocator *json_allocator_() const override {
    return this->allocator != nullptr ? this->allocator : WebScheduleEditor::json_allocator_();
  }
};

// A heap that hands out `left` bytes in all, then refuses.
struct BudgetAllocator : ArduinoJson::Allocator {
  explicit BudgetAllocator(size_t budget) : left(budget) {}
  void *allocate(size_t size) override {
    if (size > this->left)
      return nullptr;
    this->left -= size;
    return malloc(size);
  }
  void deallocate(void *ptr) override { free(ptr); }
  void *reallocate(void *ptr, size_t size) override {
    if (size > this->left)
      return nullptr;
    this->left -= size;
    return realloc(ptr, size);
  }
  size_t left;
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
  std::string header(const char *name) const {
    for (const auto &header : this->headers) {
      if (header.first == name)
        return header.second;
    }
    return "";
  }
};

// On every evening at 19:00, off at 23:30: contract.json's porch.
inline std::string porch(const char *name = "Porch", const char *relay = "relay_3", bool enabled = true) {
  std::string week;
  for (const char *day : schedules::DAY_NAMES)
    week += std::string(week.empty() ? "" : ",") + "\"" + day +
            R"(":[{"at":"19:00","mode":"on"},{"at":"23:30","mode":"off"}])";
  const std::string relay_is = std::string(R"({"type":"relay","id":")") + relay + R"(","value":{"type":)";
  return std::string(R"({"name":")") + name + R"(","enabled":)" + (enabled ? "true" : "false") +
         R"(,"modes":[{"id":"on","name":"On","color":"green","actions":[)" + relay_is + R"("on"}}]},)" +
         R"({"id":"off","name":"Off","color":"grey","actions":[)" + relay_is + R"("off"}}]}],"week":{)" + week + "}}";
}

// Living Room's eco from Wednesday 08:30, 21.5 from 17:00.
inline std::string living_room_week(const char *name = "Living room week", bool enabled = true) {
  return std::string(R"({"name":")") + name + R"(","enabled":)" + (enabled ? "true" : "false") +
         R"(,"modes":[{"id":"eco","name":"Eco","actions":[)"
         R"({"type":"thermostat","id":"living-room","value":{"type":"preset","preset":"eco"}}]},)"
         R"({"id":"warm","name":"Warm","actions":[)"
         R"({"type":"thermostat","id":"living-room","value":{"type":"target","target":21.5}}]}],)"
         R"("week":{"wed":[{"at":"08:30","mode":"eco"},{"at":"17:00","mode":"warm"}]}})";
}

// @p json with @p fields put first, e.g. with(porch(), R"("id":"porch")").
inline std::string with(const std::string &json, const std::string &fields) {
  std::string out = json;
  out.insert(1, fields + ",");
  return out;
}

// The editor over the process's engine and hub, over a temporary directory, reached through
// the harness's web_server_base stand-in exactly as a request from the network would reach it.
class Editor : public ::testing::Test {
 protected:
  void SetUp() override {
    entities();
    hub();
    LogCapture::instance().clear();
    mkdir(".storage", 0755);
    char folder[] = ".storage/sched-XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->base_path = folder;
    this->fresh();
    this->editor = std::make_unique<TestEditor>(&this->base, &engine());
    this->editor->setup();
  }

  void TearDown() override {
    engine().forget();
    engine().reset_seams();
    switch_hold::set_holder(&hub());
    storage().path = this->base_path;
    remove_tree(this->base_path);
  }

  // A device with no schedule, its thermostats as contract.json has them, booted, its clock
  // reading @p now (0 for none valid).
  void fresh(int64_t now = wednesday()) {
    engine().forget();
    engine().reset_seams();
    switch_hold::set_holder(&hub());
    storage().path = this->base_path;
    remove_tree(this->folder());
    reset_entities();
    reset_thermostats();
    engine().boot();
    this->set_clock(now);
  }

  // The clock reads @p when, and the loop has run since.
  void set_clock(int64_t when) {
    engine().local = when;
    engine().mono += 1000;
    engine().tick();
    engine().run_later();
  }

  // A boot over the files as they are now, the clock as it was.
  void reboot() {
    const int64_t when = engine().local;
    engine().boot();
    this->set_clock(when);
  }

  std::string folder() const { return this->base_path + "/schedules"; }

  std::vector<std::string> files() const {
    std::vector<std::string> names;
    for (const std::string &name : list_dir(this->folder())) {
      if (name[0] != '.')
        names.push_back(name);
    }
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
    return this->request(method, "/schedule-editor/api/" + route, body, content_type, origin);
  }
  Reply get(const std::string &route) { return this->call(HTTP_GET, route); }
  Reply post(const std::string &route, const std::string &body = "") { return this->call(HTTP_POST, route, body); }

  // Creates a schedule and returns its id, "" when the API refused it.
  std::string create(const std::string &json) {
    Reply reply = this->post("save", json);
    EXPECT_EQ(reply.code, 200) << reply.body;
    return reply["id"] | "";
  }

  // The Host every case is addressed to; a page on another site says so by carrying an Origin
  // that is not this.
  static constexpr const char *HOST = "device.local";

  std::string base_path;
  web_server_base::WebServerBase base;
  std::unique_ptr<TestEditor> editor;
  Entities &e = entities();
};

}  // namespace esphome::web_schedule_editor::testing
