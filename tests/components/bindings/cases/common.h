#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/bindings/bindings.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/switch_hold/switch_hold.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

namespace esphome::bindings::testing {

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

// Stands in for climate_hub: holds the switches the case puts in the table, by name.
class FakeHolder : public switch_hold::SwitchHolder {
 public:
  std::map<const switch_::Switch *, std::string> held;
  std::string holder_of(const switch_::Switch *sw) const override {
    auto it = this->held.find(sw);
    return it == this->held.end() ? std::string() : it->second;
  }
  // What the hub does once it has let go of `sw`.
  void release(switch_::Switch *sw) {
    this->held.erase(sw);
    switch_hold::notify_released(sw);
  }
};

// Every warning and info line the process logs. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> warnings;
  std::vector<std::string> infos;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  bool has(const char *needle) const { return contains(this->warnings, needle); }
  bool has_info(const char *needle) const { return contains(this->infos, needle); }

 protected:
  static bool contains(const std::vector<std::string> &lines, const char *needle) {
    return std::any_of(lines.begin(), lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    if (level == ESPHOME_LOG_LEVEL_WARN)
      static_cast<LogCapture *>(self)->warnings.emplace_back(message, len);
    if (level == ESPHOME_LOG_LEVEL_INFO)
      static_cast<LogCapture *>(self)->infos.emplace_back(message, len);
  }
};

// Registered once: App keeps the pointers for the life of the process and the manager finds
// them by the hash of their object id.
struct Entities {
  binary_sensor::BinarySensor in1;
  binary_sensor::BinarySensor in2;
  FakeSwitch relay1;
  FakeSwitch relay2;
};

inline Entities &entities() {
  static Entities *instance = [] {
    auto *e = new Entities();
    App.register_binary_sensor(&e->in1, "Input 1", fnv1_hash("input_1"), 0);
    App.register_binary_sensor(&e->in2, "Input 2", fnv1_hash("input_2"), 0);
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    App.register_switch(&e->relay2, "Relay 2", fnv1_hash("relay_2"), 0);
    return e;
  }();
  return *instance;
}

inline const uint32_t IN_1 = fnv1_hash("input_1");
inline const uint32_t IN_2 = fnv1_hash("input_2");
inline const uint32_t RELAY_1 = fnv1_hash("relay_1");
inline const uint32_t RELAY_2 = fnv1_hash("relay_2");
inline const uint32_t NO_SUCH = fnv1_hash("no_such_entity");

// One manager per test. They outlive the process: an input keeps a callback into each one it
// was subscribed to, and production never destroys the component either. The fixture unbinds
// everything so a stale manager sees nothing to do.
class Bindings : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::vector<std::unique_ptr<BindingsManager>> all;
    all.push_back(std::make_unique<BindingsManager>());
    this->manager = all.back().get();
    LogCapture::instance().warnings.clear();
    LogCapture::instance().infos.clear();
    switch_hold::set_holder(&this->holder);
    Entities &e = entities();
    e.in1.publish_state(false);
    e.in2.publish_state(false);
    for (FakeSwitch *sw : {&e.relay1, &e.relay2}) {
      sw->publish_state(false);
      sw->writes = 0;
    }
  }
  void TearDown() override {
    this->manager->remove_binding(RELAY_1);
    this->manager->remove_binding(RELAY_2);
    switch_hold::set_holder(nullptr);
  }

  static void press(binary_sensor::BinarySensor &input) {
    input.publish_state(true);
    input.publish_state(false);
  }
  static LogCapture &log() { return LogCapture::instance(); }

  BindingsManager *manager{nullptr};
  FakeHolder holder;
  Entities &e = entities();
};

}  // namespace esphome::bindings::testing
