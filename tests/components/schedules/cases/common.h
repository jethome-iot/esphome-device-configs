#pragma once
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/climate_hub/climate_hub.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/schedules/schedule_config.h"
#include "esphome/components/schedules/schedule_engine.h"
#include "esphome/components/schedules/week_clock.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome::schedules::testing {

using climate_hub::HubMode;

// A mounted directory, the way littlefs_storage presents the partition.
class FakeStorage : public filesystem_storage_abstract::FilesystemStorageAbstract {
 public:
  std::string path;
  bool mounted{true};
  bool is_mounted() const override { return this->mounted; }
  const std::string &get_base_path() const override { return this->path; }
  const char *get_filesystem_type() const override { return "Directory"; }
  bool request_format() override { return false; }
};

// What the process logs from here on. Registered once: the logger keeps its listeners, and has
// room for one.
class LogCapture {
 public:
  std::vector<std::string> lines;
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
    this->lines.clear();
    this->warnings.clear();
  }
  bool has(const std::string &needle) const { return has_in(this->lines, needle); }
  bool warned(const std::string &needle) const { return has_in(this->warnings, needle); }

 protected:
  static bool has_in(const std::vector<std::string> &lines, const std::string &needle) {
    return std::any_of(lines.begin(), lines.end(),
                       [&needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    auto *capture = static_cast<LogCapture *>(self);
    capture->lines.emplace_back(message, len);
    if (level <= ESPHOME_LOG_LEVEL_WARN)
      capture->warnings.emplace_back(message, len);
  }
};

// Remembers every write; the state follows it like an optimistic template switch. on_write runs
// after each, where a YAML on_turn_on would.
class FakeSwitch : public switch_::Switch {
 public:
  std::vector<bool> writes;
  std::function<void()> on_write;

 protected:
  void write_state(bool state) override {
    this->writes.push_back(state);
    this->publish_state(state);
    if (this->on_write)
      this->on_write();
  }
};

// Local clock seconds in the week of 2026-10-26, a Monday: `day` 0 is that Monday.
inline int64_t at_local(int day, int hour, int minute, int second = 0) {
  return (days_from_civil(2026, 10, 26) + day) * 86400 + hour * 3600 + minute * 60 + second;
}

// Minutes since Monday 00:00.
inline uint16_t wm(int day, int hour, int minute) { return static_cast<uint16_t>(day * 1440 + hour * 60 + minute); }

// The engine with its seams taken: the local clock is `local` (0 for none valid yet), the
// loop's count `mono`, a job for the loop waits in `jobs`, a file can be made undeletable, the
// encoder given a smaller cap or a heap that runs out.
class FakeEngine : public ScheduleEngine {
 public:
  int64_t local{0};
  uint64_t mono{5000000};
  std::vector<std::function<void()>> jobs;
  std::vector<std::string> undeletable;
  size_t max_file_bytes{MAX_FILE_BYTES};
  ArduinoJson::Allocator *json_allocator{nullptr};

  void tick() { this->tick_(); }
  // Both clocks move on, a tick a second, as the loop runs.
  void run(int seconds) {
    for (int i = 0; i < seconds; i++) {
      this->local++;
      this->mono += 1000;
      this->tick_();
    }
  }
  // The loop stalls: both clocks move on by `seconds`, then one tick.
  void stall(int seconds) {
    this->local += seconds;
    this->mono += static_cast<uint64_t>(seconds) * 1000;
    this->tick_();
  }
  // One second of loop, at whose end the local clock reads `when`: a set clock, a zone, DST.
  void jump_to(int64_t when) {
    this->local = when;
    this->mono += 1000;
    this->tick_();
  }
  // Runs what waits for the loop, and what that queues. The count it ran.
  size_t run_later() {
    size_t ran = 0;
    while (!this->jobs.empty()) {
      auto now = std::move(this->jobs);
      this->jobs.clear();
      for (auto &job : now) {
        job();
        ran++;
      }
    }
    return ran;
  }
  // An engine a test is done with: the entities and the hub keep a callback into it, so it
  // stays, but drives nothing.
  void forget() {
    this->entries_.clear();
    this->jobs.clear();
  }
  bool active(const std::string &id) const {
    const Entry *entry = this->find_(id);
    return entry != nullptr && entry->active;
  }

 protected:
  ESPTime clock_now_() override { return this->local == 0 ? ESPTime{} : ESPTime::from_epoch_utc(this->local); }
  uint64_t mono_ms_() override { return this->mono; }
  void later_(std::function<void()> &&job) override { this->jobs.push_back(std::move(job)); }
  EncodeError encode_(const ScheduleConfig &config, std::string *json) const override {
    if (this->json_allocator == nullptr)
      return config.encode(json, this->max_file_bytes);
    return config.encode(json, this->max_file_bytes, this->json_allocator);
  }
  bool remove_file_(const std::string &path) override {
    for (const std::string &name : this->undeletable) {
      if (path.size() >= name.size() && path.compare(path.size() - name.size(), name.size(), name) == 0)
        return false;
    }
    return ScheduleEngine::remove_file_(path);
  }
};

// The entities the schedules under test may name. Registered once: App keeps the pointers for
// the life of the process, and the engine and the hub find them by object id.
struct Entities {
  sensor::Sensor room;
  FakeSwitch relay1;
  FakeSwitch relay2;
  FakeSwitch boiler;
  // internal: true in YAML; nothing outside the firmware may name it.
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
    App.register_switch(&e->boiler, "Boiler", fnv1_hash("boiler"), 0);
    App.register_switch(&e->hidden, "Hidden", fnv1_hash("hidden"), 1u << ENTITY_FIELD_INTERNAL_SHIFT);
    return e;
  }();
  return *instance;
}

inline void reset_entities() {
  Entities &e = entities();
  e.room.state = NAN;
  for (FakeSwitch *sw : {&e.relay1, &e.relay2, &e.boiler, &e.hidden}) {
    sw->on_write = nullptr;
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

// Empties `path`, the folders a case made there included, and removes it.
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
    h->set_max_controllers(2);
    h->setup();
    return h;
  }();
  return *instance;
}

inline FakeStorage &storage() {
  static FakeStorage *instance = new FakeStorage();
  return *instance;
}

inline climate_hub::PresetConfig preset(const char *name, float setpoint, optional<HubMode> mode = nullopt) {
  climate_hub::PresetConfig p;
  p.name = name;
  p.setpoint = setpoint;
  p.mode = mode;
  return p;
}

// Heats on `relay` from `room`, at 21, with eco (18), comfort (22), away (12, mode off) and
// boost (25, mode heat).
inline climate_hub::ClimateConfig thermostat(const char *name = "Living room", const char *relay = "boiler",
                                             bool enabled = true) {
  climate_hub::ClimateConfig c;
  c.name = name;
  c.enabled = enabled;
  c.sensor_id = "room";
  c.heat.relay_id = relay;
  c.mode = HubMode::HEAT;
  c.setpoint = 21.f;
  c.presets = {preset("Eco", 18.f), preset("Comfort", 22.f), preset("Away", 12.f, HubMode::OFF),
               preset("Boost", 25.f, HubMode::HEAT)};
  return c;
}

inline const climate_hub::ClimateConfig &stored(const std::string &id) { return *hub().store().get(id); }

// A switch point for a document built in C++.
struct P {
  uint16_t minute;
  ValueType type;
  const char *preset;
  float target;
};
inline P on_at(uint16_t minute) { return {minute, ValueType::ON, "", NAN}; }
inline P off_at(uint16_t minute) { return {minute, ValueType::OFF, "", NAN}; }
inline P preset_at(uint16_t minute, const char *key) { return {minute, ValueType::PRESET, key, NAN}; }
inline P target_at(uint16_t minute, float target) { return {minute, ValueType::TARGET, "", target}; }

inline ScheduleConfig make(const char *name, TargetType type, const char *target, std::initializer_list<P> points,
                           bool enabled = true) {
  ScheduleConfig c;
  c.name = name;
  c.enabled = enabled;
  c.target_type = type;
  c.target_id = target;
  for (const P &p : points)
    c.add_point(p.minute, p.type, p.preset, p.target);
  return c;
}

inline ScheduleConfig relay_doc(const char *name, const char *relay, std::initializer_list<P> points,
                                bool enabled = true) {
  return make(name, TargetType::RELAY, relay, points, enabled);
}

inline ScheduleConfig thermostat_doc(const char *name, const char *id, std::initializer_list<P> points,
                                     bool enabled = true) {
  return make(name, TargetType::THERMOSTAT, id, points, enabled);
}

// On at 08:00 and off at 20:00 every day.
inline ScheduleConfig daily(const char *name = "Porch", const char *relay = "relay_1", bool enabled = true) {
  ScheduleConfig c = relay_doc(name, relay, {}, enabled);
  for (int day = 0; day < 7; day++) {
    c.add_point(wm(day, 8, 0), ValueType::ON);
    c.add_point(wm(day, 20, 0), ValueType::OFF);
  }
  return c;
}

inline bool from_json(const std::string &json, ScheduleConfig *out, std::string *error, bool require_id = false) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    *error = "test JSON does not parse";
    return false;
  }
  return out->deserialize(doc.as<JsonObjectConst>(), require_id, error);
}

inline std::string to_json(const ScheduleConfig &config) {
  JsonDocument doc;
  config.serialize(doc.to<JsonObject>());
  std::string out;
  serializeJson(doc, out);
  return out;
}

inline std::string read_file(const std::string &path) {
  FILE *file = fopen(path.c_str(), "r");
  if (file == nullptr)
    return "";
  std::string data;
  char chunk[256];
  size_t got;
  while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0)
    data.append(chunk, got);
  fclose(file);
  return data;
}

inline void write_file(const std::string &path, const std::string &data) {
  FILE *file = fopen(path.c_str(), "w");
  ASSERT_NE(nullptr, file) << path;
  fwrite(data.data(), 1, data.size(), file);
  fclose(file);
}

inline bool file_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

// A schedule as a file holds it, on `relay`, on at 08:00 and off at 20:00 on Monday.
inline std::string file_doc(const char *id, const char *name, const char *relay = "relay_1", bool enabled = true,
                            int version = 1) {
  char buf[512];
  snprintf(buf, sizeof(buf),
           R"({"version":%d,"id":"%s","name":"%s","enabled":%s,"target":{"type":"relay","id":"%s"},)"
           R"("week":{"mon":[{"at":"08:00","value":{"type":"on"}},{"at":"20:00","value":{"type":"off"}}]}})",
           version, id, name, enabled ? "true" : "false", relay);
  return buf;
}

// Every case gets an engine of its own over an empty folder, and a hub without thermostats.
class EngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    entities();
    reset_entities();
    hub();
    LogCapture::instance().clear();
    mkdir(".storage", 0755);
    char folder[] = ".storage/sched-XXXXXX";
    ASSERT_NE(nullptr, mkdtemp(folder));
    this->base_ = folder;
    storage().path = this->base_;
    storage().mounted = true;
    this->engine = &this->new_engine();
  }

  void TearDown() override {
    for (FakeEngine *e : this->engines_)
      e->forget();
    std::vector<std::string> ids;
    for (const auto &config : hub().store().all())
      ids.push_back(config->id);
    for (const std::string &id : ids)
      hub().remove(id);
    remove_tree(this->base_);
  }

  // Engines outlive the case: the entities and the hub keep a callback into every one that set
  // up. forget() empties them, so later cases see nothing move.
  FakeEngine &new_engine() {
    static std::vector<std::unique_ptr<FakeEngine>> all;
    all.push_back(std::make_unique<FakeEngine>());
    FakeEngine &e = *all.back();
    e.set_storage(&storage());
    e.set_folder_path("schedules");
    e.set_max_schedules(4);
    this->engines_.push_back(&e);
    return e;
  }

  // Sets the engine up over the folder, with a valid clock at `when` (0 for none), and ticks once.
  void boot(int64_t when = 0) {
    this->engine->local = when;
    this->engine->setup();
    this->engine->tick();
  }
  // What the next boot sees: a new engine over the same files, the clock where it was.
  void reboot() {
    const int64_t when = this->engine->local;
    this->engine->forget();
    this->engine = &this->new_engine();
    this->boot(when);
  }

  std::string folder() const { return this->base_ + "/schedules"; }
  std::string file_of(const std::string &id) const { return this->folder() + "/" + id + ".json"; }

  Result create(const ScheduleConfig &config) {
    Result result = this->engine->create(config);
    EXPECT_TRUE(result.ok) << result.error;
    return result;
  }
  // A thermostat the hub runs, `id` from its name.
  std::string add_thermostat(const climate_hub::ClimateConfig &config) {
    climate_hub::Result result = hub().create(config);
    EXPECT_TRUE(result.ok) << result.error;
    return result.id;
  }
  ScheduleState state(const std::string &id) const { return this->engine->status(id).state; }

  FakeEngine *engine{nullptr};
  Entities &e = entities();
  std::string base_;
  std::vector<FakeEngine *> engines_;
};

}  // namespace esphome::schedules::testing
