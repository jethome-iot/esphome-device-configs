#pragma once
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "esphome/components/climate/climate.h"
#include "esphome/components/climate_hub/climate_config.h"
#include "esphome/components/climate_hub/climate_hub.h"
#include "esphome/components/climate_hub/entity_lookup.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#ifdef __SANITIZE_ADDRESS__
// AddressSanitizer's count of the heap in use; GCC ships no header that declares it.
extern "C" size_t __sanitizer_get_current_allocated_bytes();
#endif

namespace esphome::climate_hub::testing {

// 2^32 ms, about 49.7 days: where a 32-bit millis() count starts again from zero.
constexpr uint64_t MILLIS_WRAP = 1ull << 32;

#ifdef __SANITIZE_ADDRESS__
// Bytes the process holds on the heap right now. Only the sanitizer's allocator says exactly:
// glibc counts a small block waiting in its cache as in use.
inline size_t heap_in_use() { return __sanitizer_get_current_allocated_bytes(); }
#endif

// Remembers every write; the state follows it like an optimistic template switch.
class FakeSwitch : public switch_::Switch {
 public:
  int writes{0};

 protected:
  void write_state(bool state) override {
    this->writes++;
    this->publish_state(state);
  }
};

// A climate the YAML declared: the hub may not hand its name to a thermostat.
class YamlClimate : public climate::Climate {
 protected:
  climate::ClimateTraits traits() override {
    climate::ClimateTraits traits;
    traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
    return traits;
  }
  void control(const climate::ClimateCall &) override {}
};

// Every line the process logs from here on. Registered once: the logger keeps its listeners.
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
  bool has(const std::string &needle) const {
    return std::any_of(this->lines.begin(), this->lines.end(),
                       [&needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t, const char *, const char *message, size_t len) {
    static_cast<LogCapture *>(self)->lines.emplace_back(message, len);
  }
};

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

// Hands out `budget` allocations, then refuses every one: a heap that runs out mid-document.
class CountdownAllocator : public ArduinoJson::Allocator {
 public:
  explicit CountdownAllocator(int budget) : budget_(budget) {}
  void *allocate(size_t size) override { return this->budget_-- > 0 ? malloc(size) : nullptr; }
  void deallocate(void *ptr) override { free(ptr); }
  void *reallocate(void *ptr, size_t new_size) override {
    return this->budget_-- > 0 ? realloc(ptr, new_size) : nullptr;
  }

 protected:
  int budget_;
};

// The hub with its seams taken: the clock is `ms`, loop jobs and reconnects are counted, a file
// can be made undeletable, and the encoder given a smaller cap or a heap that runs out.
class TestHub : public ClimateHub {
 public:
  uint64_t ms{100000};
  int loop_jobs{0};
  int resyncs{0};
  std::vector<std::string> undeletable;
  size_t max_file_bytes{CONFIG_MAX_BYTES};
  ArduinoJson::Allocator *json_allocator{nullptr};

  uint64_t now_ms() const override { return this->ms; }
  bool run_on_loop(std::function<bool()> &&job) override {
    this->loop_jobs++;
    return ClimateHub::run_on_loop(std::move(job));
  }

  // As if App had room for only `count` of the pool's entities: the others are set aside, unused,
  // until reset(). Only before any of them runs.
  void shorten_pool(size_t count) {
    while (this->slots_.size() > count) {
      this->free_.erase(std::find(this->free_.begin(), this->free_.end(), this->slots_.back()));
      this->set_aside_.push_back(this->slots_.back());
      this->slots_.pop_back();
    }
  }

  // Back to a hub that has not loaded anything, its pool as setup() left it: every slot free,
  // hidden under the placeholder, in order. App keeps the entities, so they are reused rather
  // than registered again.
  void reset() {
    this->slots_.insert(this->slots_.end(), this->set_aside_.rbegin(), this->set_aside_.rend());
    this->set_aside_.clear();
    for (Slot *slot : this->slots_) {
      this->stop_(slot);
      slot->entity.park(this->entity_fields_);
    }
    this->free_.assign(this->slots_.begin(), this->slots_.end());
    this->claims_.clear();
    this->relay_history_.clear();
    // The subscriptions stay, as they do on a device; what they heard does not survive a boot.
    for (auto &sub : this->sensor_subs_)
      sub->last = Reading{};
    this->store_.clear();
    this->dirty_.clear();
    this->waiting_.clear();
    this->autotunes_.clear();
    // The stops above let go of relays and entities no mutator will start anyone on or announce.
    this->freed_.clear();
    this->entity_freed_ = false;
    this->cancel_timeout("ha_resync");
    this->ms = 100000;
    this->resyncs = 0;
    this->loop_jobs = 0;
    this->undeletable.clear();
    this->max_file_bytes = CONFIG_MAX_BYTES;
    this->json_allocator = nullptr;
    this->ha_resync_delay_ms_ = 20;
  }

  void register_pool() { this->build_pool_(); }
  HubClimate *slot_entity(size_t index) { return &this->slots_[index]->entity; }
  size_t slot_count() const { return this->slots_.size(); }
  size_t free_count() const { return this->free_.size(); }
  size_t sensor_subscriptions() const { return this->sensor_subs_.size(); }
  HubClimate *entity_of(const std::string &id) {
    Slot *slot = this->slot_for_(id);
    return slot == nullptr ? nullptr : &slot->entity;
  }
  ControllerRuntime *runtime_of(const std::string &id) {
    Slot *slot = this->slot_for_(id);
    return slot == nullptr ? nullptr : &slot->runtime;
  }
  const RelayClaim *claim(const std::string &relay) const {
    auto it = this->claims_.find(relay);
    return it == this->claims_.end() ? nullptr : it->second.get();
  }
  bool dirty(const std::string &id) const { return this->dirty_.count(id) != 0; }
  // Every reason kept, stale or not: waiting_reason() would hide one the hub forgot to drop.
  size_t reasons_kept() const { return this->waiting_.size(); }

 protected:
  // shorten_pool()'s, last slot first.
  std::vector<Slot *> set_aside_;

  void resync_home_assistant_() override { this->resyncs++; }
  bool remove_file_(const std::string &path) override {
    for (const std::string &name : this->undeletable) {
      if (path.size() >= name.size() && path.compare(path.size() - name.size(), name.size(), name) == 0)
        return false;
    }
    return ClimateHub::remove_file_(path);
  }
  EncodeError encode_(const ClimateConfig &config, std::string *json) const override {
    if (this->json_allocator == nullptr)
      return config.encode(json, this->max_file_bytes);
    return config.encode(json, this->max_file_bytes, this->json_allocator);
  }
};

// The entities the documents under test may name. Registered once: App keeps the pointers for
// the life of the process, and the hub finds them by object id.
struct Entities {
  sensor::Sensor room;
  sensor::Sensor floor;
  // internal: true in YAML; nothing outside the firmware may name it.
  sensor::Sensor hidden;
  // Visible, but not in °C: no thermostat input.
  sensor::Sensor uptime;
  sensor::Sensor counter;
  FakeSwitch relay1;
  FakeSwitch relay2;
  FakeSwitch relay3;
  YamlClimate hall;
  // internal: true in YAML; the web server still answers its name.
  YamlClimate cellar;
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
    App.register_sensor(&e->hidden, "Hidden", fnv1_hash("hidden"), celsius | (1u << ENTITY_FIELD_INTERNAL_SHIFT));
    App.register_sensor(&e->uptime, "Uptime", fnv1_hash("uptime"), unit_field("s"));
    App.register_sensor(&e->counter, "Counter", fnv1_hash("counter"), 0);
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    App.register_switch(&e->relay2, "Relay 2", fnv1_hash("relay_2"), 0);
    App.register_switch(&e->relay3, "Relay 3", fnv1_hash("relay_3"), 0);
    App.register_climate(&e->hall, "Hall", fnv1_hash("hall"), 0);
    App.register_climate(&e->cellar, "Cellar", fnv1_hash("cellar"), 1u << ENTITY_FIELD_INTERNAL_SHIFT);
    return e;
  }();
  return *instance;
}

inline FakeStorage &storage() {
  static FakeStorage *instance = new FakeStorage();
  return *instance;
}

// One hub for the whole process. App has room for exactly one pool, so this one registers
// its pool the moment it exists, before any other hub a case builds can take the room.
inline TestHub &hub() {
  static TestHub *instance = [] {
    auto *h = new TestHub();
    h->set_storage(&storage());
    h->set_folder_path("climates");
    h->set_max_controllers(4);
    h->set_icon_index(1);
    h->register_pool();
    return h;
  }();
  return *instance;
}

inline void reset_entities() {
  Entities &e = entities();
  for (sensor::Sensor *s : {&e.room, &e.floor, &e.hidden, &e.uptime, &e.counter}) {
    s->state = NAN;
    s->set_has_state(false);
  }
  for (FakeSwitch *sw : {&e.relay1, &e.relay2, &e.relay3}) {
    sw->publish_state(false);
    sw->writes = 0;
  }
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

// A document as a file holds it: `cool` "" for heat only, `heat` "" for cool only. Two enabled
// thermostats on one relay come only from files.
inline std::string file_doc(const char *id, const char *name, const char *heat, const char *cool = "",
                            bool enabled = true, const char *sensor = "room") {
  const char *mode = *heat == '\0' ? "cool" : *cool == '\0' ? "heat" : "heat_cool";
  char buf[512];
  snprintf(buf, sizeof(buf),
           R"({"version":1,"id":"%s","name":"%s","enabled":%s,"kind":"bang_bang","sensor_id":"%s",)"
           R"("heat":{"relay_id":"%s"},"cool":{"relay_id":"%s"},"mode":"%s","setpoint":21})",
           id, name, enabled ? "true" : "false", sensor, heat, cool, mode);
  return buf;
}

inline bool file_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
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

inline std::string object_id(const EntityBase &entity) {
  char buf[OBJECT_ID_MAX_LEN];
  return std::string(entity.get_object_id_to(buf));
}

// What web_server answers /climate/<name> with: the first climate by that name, hidden or not.
inline climate::Climate *web_server_match(const std::string &name) {
  for (auto *climate : App.get_climates()) {
    if (name == climate->get_name().c_str())
      return climate;
  }
  return nullptr;
}

inline std::string to_json(const ClimateConfig &config) {
  JsonDocument doc;
  config.serialize(doc.to<JsonObject>());
  std::string out;
  serializeJson(doc, out);
  return out;
}

inline bool from_json(const std::string &json, ClimateConfig *out, std::string *error, bool require_id = true) {
  JsonDocument doc;
  if (deserializeJson(doc, json))
    return false;
  return out->deserialize(doc.as<JsonObject>(), require_id, error);
}

inline ClimateConfig draft(const std::string &name, const std::string &relay = "relay_1") {
  ClimateConfig c;
  c.name = name;
  c.kind = ControlKind::PID;
  c.sensor_id = "room";
  c.heat.relay_id = relay;
  c.mode = HubMode::HEAT;
  return c;
}

// Every test gets the process-wide hub freshly reset over an empty folder of its own.
class HubTest : public ::testing::Test {
 protected:
  void SetUp() override {
    entities();
    reset_entities();
    mkdir(".storage", 0755);
    char folder[] = ".storage/climate-XXXXXX";
    ASSERT_NE(nullptr, mkdtemp(folder));
    this->base_ = folder;
    storage().path = this->base_;
    storage().mounted = true;
    hub().reset();
    hub().setup();
  }

  void TearDown() override {
    hub().reset();
    storage().path = this->base_;
    for (const std::string &name : list_dir(this->folder()))
      ::remove((this->folder() + "/" + name).c_str());
    rmdir(this->folder().c_str());
    rmdir(this->base_.c_str());
  }

  // What the next boot sees: the documents on flash, loaded by a hub that knows nothing.
  void reboot() {
    hub().on_shutdown();
    hub().reset();
    reset_entities();
    hub().setup();
  }

  // Lets the wall clock pass the reconnect delay and runs what the scheduler has due.
  static void pass_resync_delay() {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    App.scheduler.call(millis());
  }

  std::string folder() const { return this->base_ + "/climates"; }
  std::string file_of(const std::string &id) const { return this->folder() + "/" + id + ".json"; }

  Result create(const ClimateConfig &config) {
    Result result = hub().create(config);
    EXPECT_TRUE(result.ok) << result.error;
    return result;
  }

  std::string base_;
};

}  // namespace esphome::climate_hub::testing
