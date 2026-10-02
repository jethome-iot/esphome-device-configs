#pragma once
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/host/preferences.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/components/mqtt/mqtt_component.h"
#include "esphome/components/mqtt_config/mqtt_config.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

namespace esphome::mqtt_config::testing {

static const char *const NODE = "mqtt-config-test";
// The prefix the client computes on a device, from the node name and the MAC.
static const char *const NODE_PREFIX = "mqtt-config-test-a1b2c3";
static const uint32_t PREF_HASH = 0x5EC0FF1Eu;

// The compiled node name, which the client's default id and prefix come from. main.cpp never
// runs the generated setup that would set it.
inline void name_the_node() {
  static const bool named = [] {
    App.pre_setup(NODE, std::strlen(NODE), "", 0);
    return true;
  }();
  (void) named;
}

// Everything the process logs. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::pair<uint8_t, std::string>> lines;

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
                       [needle](const auto &line) { return line.second.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    static_cast<LogCapture *>(self)->lines.emplace_back(level, std::string(message, len));
  }
};

class FakeSwitch : public switch_::Switch {
 protected:
  void write_state(bool state) override { this->publish_state(state); }
};

// The entities the walk covers. Registered once: App keeps the pointers for the process.
struct Entities {
  FakeSwitch relay1;
  FakeSwitch relay2;
  FakeSwitch hidden;
  binary_sensor::BinarySensor input;
  sensor::Sensor temp3;
};

inline Entities &entities() {
  static Entities *instance = [] {
    auto *e = new Entities();
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    App.register_switch(&e->relay2, "Relay 2", fnv1_hash("relay_2"), 0);
    App.register_switch(&e->hidden, "Hidden", fnv1_hash("hidden"), 0);
    App.register_binary_sensor(&e->input, "Input 1", fnv1_hash("input_1"), 0);
    App.register_sensor(&e->temp3, "Temp 3", fnv1_hash("temp_3"), 0);
    return e;
  }();
  return *instance;
}

// NVS, RTC memory and the reset reason, which outlive one MqttConfig the way they outlive a
// boot. NVS is a byte blob here: the host preferences keep nothing over 255 bytes.
struct Board {
  std::vector<uint8_t> nvs;  // empty: nothing stored
  CrashGuardRecord rtc{};
  bool panic{false};
  bool log_listener_added{false};

  bool read(StoredMqttV1 &out) const {
    if (this->nvs.size() != sizeof(StoredMqttV1))
      return false;
    std::memcpy(&out, this->nvs.data(), sizeof(StoredMqttV1));
    return true;
  }
  void write(const StoredMqttV1 &record) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&record);
    this->nvs.assign(bytes, bytes + sizeof(StoredMqttV1));
  }
  StoredMqttV1 record() const {
    StoredMqttV1 out{};
    EXPECT_TRUE(this->read(out)) << "nothing stored";
    return out;
  }
};

// The component with its ESP-IDF seams swapped for the board above and a flash that can fail:
// the flush fails with the record either already in NVS (another key's failure, as far as
// this record can tell) or nowhere.
class TestConfig : public MqttConfig {
 public:
  TestConfig(mqtt::MQTTClientComponent *client, Board *board) : MqttConfig(client), board_(board) {}

  enum class Flush { OK, FAILED_RECORD_LANDED, FAILED_RECORD_LOST };
  Flush flush{Flush::OK};
  int stores{0};

  void set_disarm_after_ms(uint32_t ms) { this->disarm_after_ms_ = ms; }
  void log(uint8_t level, const char *tag, const std::string &line) {
    MqttConfig::on_log_(this, level, tag, line.data(), line.size());
  }
  size_t walked() const { return this->entities_.size(); }
  mqtt::MQTTComponent *walked_component(size_t i) const { return this->entities_[i].component; }
  // Nothing the scheduler holds may outlive this object.
  void forget_schedule() {
    this->cancel_timeout("mqtt-guard");
    this->cancel_interval("mqtt-cleanup");
  }
  void run_loop() { this->loop(); }

 protected:
  bool load_record_(StoredMqttV1 &out) override { return this->board_->read(out); }
  bool store_(const StoredMqttV1 &stored) override {
    this->stores++;
    if (this->flush != Flush::FAILED_RECORD_LOST)
      this->board_->write(stored);
    return this->flush == Flush::OK;
  }
  bool panic_reset_() const override { return this->board_->panic; }
  CrashGuardRecord &guard_record_() override { return this->board_->rtc; }
  void add_log_listener_() override { this->board_->log_listener_added = true; }

  Board *board_;
};

inline StoredMqttV1 record_of(const MqttRecord &record) { return to_stored(record, StoredMqttV1{}); }
inline MqttRecord enabled_record(const char *broker = "192.168.1.10") {
  MqttRecord r;
  r.enabled = true;
  r.broker = broker;
  return r;
}

template<typename T> MqttPatch patch_of(T &&fill) {
  MqttPatch patch;
  fill(patch);
  return patch;
}

// Lets the wall clock pass and runs what the scheduler has due.
inline void advance(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  App.scheduler.call(millis());
}

// A device: the client as features/mqtt.yaml compiles it, this component, and an MQTT
// component per walked entity, set up in boot order (210, 200, 100).
class MqttTest : public ::testing::Test {
 protected:
  void SetUp() override {
    name_the_node();
    entities();
    LogCapture::instance().clear();
    // make_preference() needs a backend, though the record itself lives on the board.
    host::setup_preferences();
  }
  void TearDown() override { this->shutdown(); }

  // The client in the state codegen leaves it in.
  static std::unique_ptr<mqtt::MQTTClientComponent> compiled_client() {
    auto client = std::make_unique<mqtt::MQTTClientComponent>();
    client->set_broker_address("");
    client->set_enable_on_boot(false);
    client->set_broker_port(1883);
    client->set_clean_session(true);
    client->set_discovery_info("homeassistant", mqtt::MQTT_MAC_ADDRESS_UNIQUE_ID_GENERATOR,
                               mqtt::MQTT_NONE_OBJECT_ID_GENERATOR, true, false);
    // Literal here: this build has no MAC suffix to add, so the test hands over what a device's
    // client would compute.
    client->set_topic_prefix(NODE_PREFIX, NODE);
    client->set_birth_message(mqtt::MQTTMessage{std::string(NODE) + "/status", "online", 0, true});
    client->set_last_will(mqtt::MQTTMessage{std::string(NODE) + "/status", "offline", 0, true});
    client->set_shutdown_message(mqtt::MQTTMessage{std::string(NODE) + "/status", "offline", 0, true});
    client->disable_log_message();
    client->set_reboot_timeout(0);
    return client;
  }

  // Codegen's calls, then setup in priority order. `before_setup` runs between construction
  // and setup, where a test plants what it needs.
  TestConfig &boot(const std::function<void(TestConfig &)> &before_setup = nullptr) {
    this->shutdown();
    this->client = compiled_client();
    this->config = std::make_unique<TestConfig>(this->client.get(), &this->board);
    TestConfig &c = *this->config;
    c.set_preference_hash(PREF_HASH);
    c.set_birth_template("online", 0, true);
    c.set_will_template("offline", 0, true);
    c.set_shutdown_template("offline", 0, true);

    Entities &e = entities();
    this->relay1 = std::make_unique<mqtt::MQTTSwitchComponent>(&e.relay1);
    this->relay2 = std::make_unique<mqtt::MQTTSwitchComponent>(&e.relay2);
    this->hidden = std::make_unique<mqtt::MQTTSwitchComponent>(&e.hidden);
    this->hidden->set_custom_state_topic("");  // state_topic: null
    this->input = std::make_unique<mqtt::MQTTBinarySensorComponent>(&e.input);
    c.reserve_entities(4);
    c.add_entity(this->relay1.get(), &e.relay1);
    c.add_entity(this->relay2.get(), &e.relay2);
    c.add_entity(this->hidden.get(), &e.hidden);
    c.add_entity(this->input.get(), &e.input);
    if (before_setup)
      before_setup(c);

    c.setup();
    this->client->setup();
    for (mqtt::MQTTComponent *component : this->components())
      component->call_setup();
    return c;
  }
  // The next boot: RTC and NVS stay, everything else is built again.
  TestConfig &reboot(const std::function<void(TestConfig &)> &before_setup = nullptr) {
    return this->boot(before_setup);
  }

  void shutdown() {
    if (this->config != nullptr)
      this->config->forget_schedule();
    this->config.reset();
  }

  std::vector<mqtt::MQTTComponent *> components() const {
    return {this->relay1.get(), this->relay2.get(), this->hidden.get(), this->input.get()};
  }

  // The client connects and runs resend passes until nothing is pending, as its loop would.
  void connect_and_settle() {
    this->client->connect_for_test();
    this->settle();
  }
  void settle() {
    while (this->client->process_resends_for_test() > 0) {
    }
  }
  // Discovery publishes that went out, by payload: "" for a clean, "{}" for an announce.
  size_t discovery_publishes(const char *payload) const {
    return std::count_if(this->client->published.begin(), this->client->published.end(),
                         [payload](const mqtt::MQTTClientComponent::Published &p) {
                           return p.topic.rfind("homeassistant/", 0) == 0 && p.payload == payload;
                         });
  }

  MqttConfig::UpdateResult save(const MqttPatch &patch) { return this->config->update(patch); }
  void plant(const MqttRecord &record) { this->board.write(record_of(record)); }
  JsonDocument settings_json() const {
    JsonDocument doc;
    this->config->write_settings_json(doc.to<JsonObject>());
    return doc;
  }

  Board board;
  std::unique_ptr<mqtt::MQTTClientComponent> client;
  std::unique_ptr<TestConfig> config;
  std::unique_ptr<mqtt::MQTTSwitchComponent> relay1;
  std::unique_ptr<mqtt::MQTTSwitchComponent> relay2;
  std::unique_ptr<mqtt::MQTTSwitchComponent> hidden;
  std::unique_ptr<mqtt::MQTTBinarySensorComponent> input;
};

}  // namespace esphome::mqtt_config::testing
