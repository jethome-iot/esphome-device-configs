#pragma once
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "esphome/components/automations/automation_config.h"
#include "esphome/components/automations/automation_storage.h"
#include "esphome/components/bindings/bindings.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/config_json/config_json.h"
#include "esphome/components/dir_storage/dir_storage.h"
#include "esphome/components/entity_config/entity_config.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/virtual_inputs/virtual_inputs.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

namespace esphome::virtual_inputs {

// What a failed comparison prints.
inline void PrintTo(const InputConfig &config, std::ostream *os) {
  *os << "{'" << config.source << "', on " << config.on_delay << " s, off " << config.off_delay << " s}";
}

}  // namespace esphome::virtual_inputs

namespace esphome::virtual_inputs::testing {

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

// Every warning and error the process logs. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> warnings;
  std::vector<std::string> errors;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() {
    this->warnings.clear();
    this->errors.clear();
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
    if (level == ESPHOME_LOG_LEVEL_WARN)
      capture->warnings.emplace_back(message, len);
    if (level == ESPHOME_LOG_LEVEL_ERROR)
      capture->errors.emplace_back(message, len);
  }
};

// The pool with its seams taken: the clock is `ms`, and the reconnects are counted.
class TestInputs : public VirtualInputs {
 public:
  uint32_t ms{100000};
  int resyncs{0};

  // Time passing, and the loop pass that notices it.
  void advance(uint32_t delta) {
    this->ms += delta;
    this->loop();
  }

  // Back to a pool that has loaded nothing: every slot free, hidden and without a state. App
  // keeps the entities, so they are reused rather than registered again; the subscriptions stay,
  // as they do on a device.
  void reset() {
    for (Slot *slot : this->slots_) {
      slot->config = InputConfig{};
      slot->source = nullptr;
      slot->line.reset();
      slot->entity.set_visible(false, 0);
      slot->entity.invalidate_state();
    }
    this->file_loaded_ = true;
    this->file_.set_slots(this->max_inputs_);
    this->cancel_timeout("ha_resync");
    this->ha_resync_delay_ms_ = 20;
    this->resyncs = 0;
    this->ms = 100000;
  }

  size_t pool_size() const { return this->slots_.size(); }
  size_t subscriptions() const { return this->subscribed_.size(); }
  VirtualInput &entity(size_t slot) { return this->slots_[slot]->entity; }
  bool pending(size_t slot) const { return this->slots_[slot]->line.pending(); }

 protected:
  uint32_t now_ms_() const override { return this->ms; }
  void resync_home_assistant_() override { this->resyncs++; }
};

// The rule engine with its clock taken; forget() drops the rules without touching the files.
class TestEngine : public automations::AutomationStorage {
 public:
  uint32_t ms{0};
  uint32_t now_ms() const override { return this->ms; }
  void forget() {
    this->automations_.clear();
    this->config_storage_.clear();
  }
};

// The physical inputs and the relays. Registered once, ahead of the pool, as codegen registers
// them before any setup runs: App keeps the pointers for the life of the process.
struct Entities {
  binary_sensor::BinarySensor in1;
  binary_sensor::BinarySensor in2;
  // The one input the cases give a label or an inversion: entity_config's filter stays in its
  // chain for good, and the filter drops a level it passed last time, which a boot sends again.
  binary_sensor::BinarySensor in3;
  // internal: true in YAML; nothing outside the firmware may name it.
  binary_sensor::BinarySensor hidden;
  FakeSwitch relay1;
  FakeSwitch relay2;
};

inline Entities &entities() {
  static Entities *instance = [] {
    auto *e = new Entities();
    App.register_binary_sensor(&e->in1, "Input 1", fnv1_hash("input_1"), 0);
    App.register_binary_sensor(&e->in2, "Input 2", fnv1_hash("input_2"), 0);
    App.register_binary_sensor(&e->in3, "Input 3", fnv1_hash("input_3"), 0);
    App.register_binary_sensor(&e->hidden, "Hidden", fnv1_hash("hidden"), 1u << ENTITY_FIELD_INTERNAL_SHIFT);
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    App.register_switch(&e->relay2, "Relay 2", fnv1_hash("relay_2"), 0);
    return e;
  }();
  return *instance;
}

// The relays with no preference to restore, so a boot's apply never asks for one.
inline void reset_entities() {
  Entities &e = entities();
  for (binary_sensor::BinarySensor *input : {&e.in1, &e.in2, &e.in3, &e.hidden})
    input->invalidate_state();
  for (FakeSwitch *sw : {&e.relay1, &e.relay2}) {
    sw->set_inverted(false);
    sw->restore_mode = switch_::SWITCH_ALWAYS_OFF;
    sw->publish_state(false);
    sw->writes = 0;
  }
}

// What the firmware wires together once, and outlives every test: the keeper's save timer points
// at the keeper, the entities keep callbacks into the pool, and App keeps the pool's entities.
struct Store {
  dir_storage::DirStorage storage;
  config_json::ConfigJsonKeeper keeper;
  // The same settings behind a keeper whose setup() failed: nothing it is handed reaches flash.
  config_json::ConfigJsonKeeper failed;
  entity_config::BinarySensorSettingsJson inputs;
  entity_config::SwitchSettingsJson relays;
  TestInputs pool;
};

inline Store &store() {
  static Store *instance = [] {
    entities();
    auto *s = new Store();
    s->keeper.set_storage(&s->storage);
    s->failed.set_storage(&s->storage);
    s->failed.mark_failed();
    for (config_json::ConfigJsonKeeper *k : {&s->keeper, &s->failed}) {
      k->add_settings(&s->inputs);
      k->add_settings(&s->relays);
    }
    s->relays.set_input_settings(&s->inputs);
    s->pool.set_max_inputs(4);
    s->pool.set_keeper(&s->keeper);
    s->pool.set_input_settings(&s->inputs);
    s->pool.set_switch_settings(&s->relays);
    return s;
  }();
  return *instance;
}

inline LogCapture &log() { return LogCapture::instance(); }

inline InputConfig follow_config(const char *source, uint32_t on_delay = 0, uint32_t off_delay = 0) {
  InputConfig config;
  config.source = source;
  config.on_delay = on_delay;
  config.off_delay = off_delay;
  return config;
}

inline const uint32_t RELAY_1 = fnv1_hash("relay_1");
inline const uint32_t RELAY_2 = fnv1_hash("relay_2");

// A fresh partition per test, and the firmware's boot in its priority order.
class Pool : public ::testing::Test {
 protected:
  void SetUp() override {
    Store &s = store();
    s.pool.reset();
    s.pool.set_keeper(&s.keeper);
    s.pool.set_rule_finder(nullptr);
    config_json::global_config_json_keeper = &s.keeper;
    s.keeper.cancel_pending_save();
    s.inputs.reset();
    s.relays.reset();
    s.inputs.clear_dirty();
    s.relays.clear_dirty();
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->base = folder;
    s.storage.set_base_path(this->base);
    s.storage.setup();
    reset_entities();
    log().clear();
  }

  void TearDown() override {
    for (bindings::BindingsManager *m : this->managers) {
      m->remove_binding(RELAY_1);
      m->remove_binding(RELAY_2);
    }
    for (TestEngine *engine : this->engines)
      engine->forget();
    Store &s = store();
    s.keeper.cancel_pending_save();
    s.pool.reset();
    remove_tree(this->base);
  }

  // The firmware's boot from the keeper's load on: the keeper (HARDWARE + 5), the pool
  // (HARDWARE + 2), the entity settings (HARDWARE + 1), the inputs' own setup, where they first
  // report, then bindings (DATA - 0.75) and the rules (DATA - 1), when the case asks for them.
  void boot(bool in1 = false, bool in2 = false) {
    Store &s = store();
    s.keeper.setup();
    s.pool.setup();
    s.inputs.apply();
    s.relays.apply();
    e.in1.publish_state(in1);
    e.in2.publish_state(in2);
  }
  // What the next boot sees: the files on flash, loaded by a pool that knows nothing.
  void reboot(bool in1 = false, bool in2 = false) {
    Store &s = store();
    s.keeper.save_immediate();
    s.pool.reset();
    s.inputs.reset();
    s.relays.reset();
    reset_entities();
    this->boot(in1, in2);
  }

  bindings::BindingsManager &new_manager() {
    static std::vector<std::unique_ptr<bindings::BindingsManager>> all;
    all.push_back(std::make_unique<bindings::BindingsManager>());
    this->managers.push_back(all.back().get());
    return *all.back();
  }
  TestEngine &new_engine() {
    static std::vector<std::unique_ptr<TestEngine>> all;
    all.push_back(std::make_unique<TestEngine>());
    TestEngine &engine = *all.back();
    engine.set_storage(&store().storage);
    this->engines.push_back(&engine);
    // As codegen wires the firmware's one engine.
    store().pool.set_rule_finder([&engine](uint32_t key) { return engine.rules_naming_binary_sensor(key); });
    return engine;
  }

  std::string config_dir() const { return this->base + "/config"; }
  // A rule file in the engine's folder, for its next setup().
  void write_rule(const std::string &file, const std::string &json) {
    const std::string folder = this->base + "/automations";
    mkdir(folder.c_str(), 0755);
    std::ofstream out(folder + "/" + file);
    out << json;
  }
  void write(const std::string &file, const std::string &json) {
    mkdir(this->config_dir().c_str(), 0755);
    std::ofstream out(this->config_dir() + "/" + file);
    out << json;
  }
  std::string read(const std::string &file) const {
    std::ifstream in(this->config_dir() + "/" + file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }
  bool exists(const std::string &file) const {
    struct stat st;
    return stat((this->config_dir() + "/" + file).c_str(), &st) == 0;
  }

  static void remove_tree(const std::string &path) {
    DIR *dir = opendir(path.c_str());
    if (dir != nullptr) {
      while (struct dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
          continue;
        const std::string child = path + "/" + name;
        struct stat st;
        if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
          chmod(child.c_str(), 0755);
          remove_tree(child);
        } else {
          remove(child.c_str());
        }
      }
      closedir(dir);
    }
    rmdir(path.c_str());
  }

  // Lets the wall clock pass the reconnect delay and runs what the scheduler has due.
  static void pass_resync_delay() {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    App.scheduler.call(millis());
  }

  static TestInputs &pool() { return store().pool; }

  std::string base;
  std::vector<bindings::BindingsManager *> managers;
  std::vector<TestEngine *> engines;
  Entities &e = entities();
};

}  // namespace esphome::virtual_inputs::testing
