#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/bindings/bindings.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/switch/switch.h"
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

// Every warning the process logs. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> warnings;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  bool has(const char *needle) const {
    return std::any_of(this->warnings.begin(), this->warnings.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    if (level == ESPHOME_LOG_LEVEL_WARN)
      static_cast<LogCapture *>(self)->warnings.emplace_back(message, len);
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
  }

  static void press(binary_sensor::BinarySensor &input) {
    input.publish_state(true);
    input.publish_state(false);
  }
  static LogCapture &log() { return LogCapture::instance(); }

  BindingsManager *manager{nullptr};
  Entities &e = entities();
};

}  // namespace esphome::bindings::testing
