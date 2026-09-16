#pragma once
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/automations/automation_config.h"
#include "esphome/components/automations/automation_storage.h"
#include "esphome/components/automations/runtime_automation.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

namespace esphome::automations::testing {

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

// Only has to exist: the engine asks has_rtc() before it builds a cron trigger.
class FakeClock : public time::RealTimeClock {
 public:
  void update() override {}
};

// Holds the delays instead of handing them to the scheduler, so a test fires them itself.
class FakeEngine : public AutomationStorage {
 public:
  struct Delay {
    uint32_t id;
    uint32_t ms;
    std::function<void()> f;
  };
  std::vector<Delay> delays;

  void schedule_delay(uint32_t id, uint32_t delay_ms, std::function<void()> &&f) override {
    this->delays.push_back({id, delay_ms, std::move(f)});
  }
  void cancel_delay(uint32_t id) override {
    for (auto it = this->delays.begin(); it != this->delays.end(); ++it) {
      if (it->id == id) {
        this->delays.erase(it);
        return;
      }
    }
  }
  // Runs the oldest pending delay. False when there is none.
  bool fire_next() {
    if (this->delays.empty())
      return false;
    Delay d = std::move(this->delays.front());
    this->delays.erase(this->delays.begin());
    d.f();
    return true;
  }
  void with_clock() { this->set_time_source(&this->clock_); }

 protected:
  FakeClock clock_;
};

// The entities the rules under test may name. Registered once: App keeps the pointers for the
// life of the process and the engine finds them by the hash of their object id.
struct Entities {
  binary_sensor::BinarySensor in1;
  binary_sensor::BinarySensor in2;
  sensor::Sensor temp;
  FakeSwitch relay1;
  FakeSwitch relay2;
};

inline Entities &entities() {
  static Entities *instance = [] {
    auto *e = new Entities();
    App.register_binary_sensor(&e->in1, "In 1", fnv1_hash("in_1"), 0);
    App.register_binary_sensor(&e->in2, "In 2", fnv1_hash("in_2"), 0);
    App.register_sensor(&e->temp, "Temp", fnv1_hash("temp"), 0);
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    App.register_switch(&e->relay2, "Relay 2", fnv1_hash("relay_2"), 0);
    return e;
  }();
  return *instance;
}

// Back to the state a fresh boot would have, as far as a test can tell.
inline void reset_entities() {
  Entities &e = entities();
  e.in1.publish_state(false);
  e.in2.publish_state(false);
  e.temp.state = NAN;
  for (FakeSwitch *sw : {&e.relay1, &e.relay2}) {
    sw->publish_state(false);
    sw->writes = 0;
  }
}

template<typename T> bool load(const char *json, T &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    ADD_FAILURE() << "test JSON does not parse: " << json;
    return false;
  }
  JsonObject obj = doc.as<JsonObject>();
  return out.deserialize(obj);
}

template<typename T> std::string dump(const T &config) {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  config.serialize(obj);
  std::string text;
  serializeJson(doc, text);
  return text;
}

inline std::unique_ptr<RuntimeAutomation> build_rule(FakeEngine &engine, const char *json) {
  AutomationConfig config;
  if (!load(json, config))
    return nullptr;
  return RuntimeAutomation::build(&engine, config);
}

}  // namespace esphome::automations::testing
