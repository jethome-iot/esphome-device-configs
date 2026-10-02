#pragma once
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/dir_storage/dir_storage.h"
#include "esphome/components/host/preferences.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/components/mqtt_config/mqtt_config.h"
#include "esphome/components/mqtt_subscriptions/mqtt_subscriptions.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "entity_tables.h"

namespace esphome::mqtt_subscriptions::testing {

static const char *const NODE = "mqtt-subscriptions-test";
// The index codegen gave each unit of test.yaml's list, in that order.
static const uint8_t UOM_CELSIUS = 1;
static const uint8_t UOM_PERCENT = 2;

// The compiled node name, which the client's default id comes from. main.cpp never runs the
// generated setup that would set it.
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
  size_t count(const char *needle) const {
    return std::count_if(this->lines.begin(), this->lines.end(),
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

// What test.yaml declares, registered once: App keeps the pointers for the process. Their
// names are the ones a slot cannot take.
struct Entities {
  sensor::Sensor board;
  binary_sensor::BinarySensor input;
  text_sensor::TextSensor ip;
  FakeSwitch relay1;
  FakeSwitch relay2;
};

inline Entities &entities() {
  static Entities *instance = [] {
    auto *e = new Entities();
    App.register_sensor(&e->board, "Board temperature", fnv1_hash("board_temperature"), 0);
    App.register_binary_sensor(&e->input, "Input 1", fnv1_hash("input_1"), 0);
    App.register_text_sensor(&e->ip, "IP address", fnv1_hash("ip_address"), 0);
    App.register_switch(&e->relay1, "Relay 1", fnv1_hash("relay_1"), 0);
    App.register_switch(&e->relay2, "Relay 2", fnv1_hash("relay_2"), 0);
    return e;
  }();
  return *instance;
}

// The RTC record and the reset reason, which outlive a boot.
struct Board {
  mqtt_config::CrashGuardRecord rtc{};
  bool panic{false};
};

// mqtt_config with its ESP-IDF seams on the board above; nothing is ever stored in NVS.
class GuardConfig : public mqtt_config::MqttConfig {
 public:
  GuardConfig(mqtt::MQTTClientComponent *client, Board *board) : MqttConfig(client), board_(board) {}
  void forget_schedule() {
    this->cancel_timeout("mqtt-guard");
    this->cancel_interval("mqtt-cleanup");
  }

 protected:
  bool load_record_(mqtt_config::StoredMqttV1 & /*out*/) override { return false; }
  bool store_(const mqtt_config::StoredMqttV1 & /*stored*/) override { return true; }
  bool panic_reset_() const override { return this->board_->panic; }
  mqtt_config::CrashGuardRecord &guard_record_() override { return this->board_->rtc; }
  void add_log_listener_() override {}

  Board *board_;
};

// The component on a clock the test moves.
class TestSubscriptions : public MqttSubscriptions {
 public:
  uint32_t now{1000};
  // A filesystem that cannot answer now: out of file handles, an I/O error.
  bool fail_reads{false};
  bool fail_stat{false};
  // Memory running out while the file is built.
  bool fail_serialize{false};

  void forget_schedule() { this->cancel_interval("mqtt-subs-check"); }
  void set_check_interval(uint32_t ms) { this->check_interval_ms_ = ms; }
  EntityBase *entity(size_t slot) const { return this->running_[slot].entity; }
  sensor::Sensor *sensor(size_t slot) const { return static_cast<sensor::Sensor *>(this->entity(slot)); }
  binary_sensor::BinarySensor *binary(size_t slot) const {
    return static_cast<binary_sensor::BinarySensor *>(this->entity(slot));
  }
  text_sensor::TextSensor *text(size_t slot) const {
    return static_cast<text_sensor::TextSensor *>(this->entity(slot));
  }
  const std::string &error(size_t slot) const { return this->running_[slot].error; }
  const std::string &raw(size_t slot) const { return this->running_[slot].raw; }

 protected:
  uint32_t now_ms_() const override { return this->now; }
  Seen stat_file_() const override {
    if (!this->fail_stat)
      return MqttSubscriptions::stat_file_();
    Seen seen;
    seen.failed = true;
    return seen;
  }
  bool read_bytes_(const std::string &path, size_t size, std::string &out) const override {
    return !this->fail_reads && MqttSubscriptions::read_bytes_(path, size, out);
  }
  bool serialize_(const std::vector<SlotConfig> &slots, std::string &out) const override {
    return !this->fail_serialize && MqttSubscriptions::serialize_(slots, out);
  }
};

inline SlotConfig slot_of(const char *name, const char *topic, SlotKind kind = SlotKind::SENSOR) {
  SlotConfig slot;
  slot.enabled = true;
  slot.name = name;
  slot.topic = topic;
  slot.kind = kind;
  return slot;
}

inline JsonDocument parse(const std::string &text) {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, text)) << text;
  return doc;
}

inline std::string read_text(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

inline void write_text(const std::string &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

inline bool exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

// Everything below `path`, then `path` itself.
inline void remove_tree(const std::string &path) {
  if (DIR *dir = opendir(path.c_str())) {
    while (struct dirent *entry = readdir(dir)) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..")
        continue;
      const std::string child = path + "/" + name;
      struct stat st;
      if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        remove_tree(child);
      } else {
        std::remove(child.c_str());
      }
    }
    closedir(dir);
  }
  rmdir(path.c_str());
}

// A device over a fresh directory: the client as features/mqtt.yaml compiles it, mqtt_config
// and the slots, set up in boot order (803, 210, 200). RTC and the directory outlive a boot.
class SlotsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    name_the_node();
    entities();
    LogCapture::instance().clear();
    // mqtt_config makes a preference at setup, though nothing is stored in it here.
    host::setup_preferences();
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->storage.set_base_path(folder);
    this->storage.setup();
    ASSERT_TRUE(this->storage.is_mounted());
    this->mark = std::make_unique<esphome::testing::EntityTableMark>();
  }
  void TearDown() override {
    this->shutdown();
    this->mark->restore();
    remove_tree(this->storage.get_base_path());
  }

  TestSubscriptions &boot(const std::function<void(TestSubscriptions &)> &before_setup = nullptr) {
    this->shutdown();
    this->mark->restore();
    this->client = std::make_unique<mqtt::MQTTClientComponent>();
    this->client->set_enable_on_boot(false);
    this->client->set_clean_session(true);
    this->config = std::make_unique<GuardConfig>(this->client.get(), &this->board);
    this->config->set_preference_hash(0x5CB5u);
    this->subs = std::make_unique<TestSubscriptions>();
    TestSubscriptions &s = *this->subs;
    s.set_config(this->config.get());
    s.set_storage(&this->storage);
    s.set_folder_path("mqtt");
    s.set_max_slots(4);
    s.add_unit("°C", UOM_CELSIUS);
    s.add_unit("%", UOM_PERCENT);
    if (before_setup)
      before_setup(s);
    s.setup();
    this->config->setup();
    this->client->setup();
    return s;
  }
  // The next boot: the directory and the RTC record stay, everything else is built again.
  TestSubscriptions &reboot(const std::function<void(TestSubscriptions &)> &before_setup = nullptr) {
    return this->boot(before_setup);
  }

  // Cancelled, then left allocated: the scheduler drops a cancelled entry only once it comes
  // due, and asks the component it names whether it failed before it looks at the cancellation.
  void shutdown() {
    if (this->subs != nullptr) {
      this->subs->forget_schedule();
      (void) this->subs.release();  // NOLINT(bugprone-unused-return-value)
    }
    if (this->config != nullptr) {
      this->config->forget_schedule();
      (void) this->config.release();  // NOLINT(bugprone-unused-return-value)
    }
    this->client.reset();
  }

  std::string folder() const { return this->storage.get_base_path() + "/mqtt"; }
  std::string file() const { return this->folder() + "/subscriptions.json"; }

  // The file as a save writes it.
  void plant(const std::vector<SlotConfig> &slots) {
    std::vector<SlotConfig> all(4);
    for (size_t i = 0; i < slots.size() && i < all.size(); i++)
      all[i] = slots[i];
    std::string text;
    ASSERT_TRUE(serialize_file(all, text));
    this->plant_text(text);
  }
  void plant_text(const std::string &text) {
    mkdir(this->folder().c_str(), 0755);
    write_text(this->file(), text);
  }

  struct Answer {
    MqttSubscriptions::Result result;
    std::string message;
    bool reboot_required;
  };
  Answer post(const std::string &body) {
    JsonDocument doc = parse(body);
    Answer answer{};
    answer.result = this->subs->post(doc.as<JsonObjectConst>(), &answer.message, &answer.reboot_required);
    return answer;
  }
  JsonDocument get() {
    JsonDocument doc;
    this->subs->write_api_json(doc.to<JsonObject>());
    return doc;
  }

  void deliver(const std::string &topic, const std::string &payload) { this->client->deliver_for_test(topic, payload); }
  size_t subscriptions_to(const std::string &topic) const {
    const auto &subs = this->client->subscriptions();
    return std::count_if(subs.begin(), subs.end(),
                         [&topic](const mqtt::MQTTSubscription &s) { return s.topic == topic; });
  }

  Board board;
  dir_storage::DirStorage storage;
  std::unique_ptr<esphome::testing::EntityTableMark> mark;
  std::unique_ptr<mqtt::MQTTClientComponent> client;
  std::unique_ptr<GuardConfig> config;
  std::unique_ptr<TestSubscriptions> subs;
};

}  // namespace esphome::mqtt_subscriptions::testing
