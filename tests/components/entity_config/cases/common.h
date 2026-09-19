#pragma once
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/bindings/bindings.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/entity_config/entity_config.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

namespace esphome::entity_config::testing {

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

// Registered once: App keeps the pointers for the life of the process and the settings find
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

// Back to the compiled defaults, as far as a test can tell. A filter appended to an input
// stays in its chain, so the fixtures also flip inversion back to off before they finish.
inline void reset_entities() {
  Entities &e = entities();
  e.in1.publish_state(false);
  e.in2.publish_state(false);
  for (FakeSwitch *sw : {&e.relay1, &e.relay2}) {
    sw->set_inverted(false);
    sw->restore_mode = switch_::SWITCH_RESTORE_DEFAULT_OFF;
    sw->publish_state(false);
    sw->writes = 0;
  }
}

// One manager for the process: the component is a singleton behind global_bindings_manager
// and the inputs keep callbacks into it. Tests unbind what they bound.
inline bindings::BindingsManager &manager() {
  static bindings::BindingsManager *instance = [] {
    auto *m = new bindings::BindingsManager();
    m->setup();
    return m;
  }();
  return *instance;
}

template<typename TRecord> bool load(const char *json, TRecord &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    ADD_FAILURE() << "test JSON does not parse: " << json;
    return false;
  }
  return out.from_json(doc.as<JsonObject>(), 1);
}

// A REST body, as the dashboard hands it to update_record().
inline JsonDocument body(const char *json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok)
    ADD_FAILURE() << "test JSON does not parse: " << json;
  return doc;
}

template<typename TRecord> std::string dump(const TRecord &record) {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  record.to_json(obj, 1);
  std::string text;
  serializeJson(doc, text);
  return text;
}

}  // namespace esphome::entity_config::testing
