#include "common.h"

namespace esphome::mqtt_config::testing {

// dallas_scan's Temp N: created at setup, so codegen gave them no MQTT component.
class BridgeTest : public MqttTest {
 protected:
  // A sensor of its own per test: the bridge subscribes to it for good.
  static sensor::Sensor *fresh_sensor() { return new sensor::Sensor(); }  // NOLINT(cppcoreguidelines-owning-memory)

  TestConfig &boot_with(std::vector<sensor::Sensor *> runtime, std::vector<sensor::Sensor *> served = {}) {
    this->plant(enabled_record());
    return this->boot([this, runtime, served](TestConfig &c) {
      for (sensor::Sensor *s : served) {
        auto component = std::make_unique<mqtt::MQTTSensorComponent>(s);
        c.add_entity(component.get(), s);
        this->served_.push_back(std::move(component));
      }
      c.add_runtime_sensors([runtime]() { return runtime; });
    });
  }
  std::vector<mqtt::MQTTComponent *> bridged() const {
    std::vector<mqtt::MQTTComponent *> out;
    for (size_t i = 4 + this->served_.size(); i < this->config->walked(); i++)
      out.push_back(this->config->walked_component(i));
    return out;
  }

  std::vector<std::unique_ptr<mqtt::MQTTSensorComponent>> served_;
};

TEST_F(BridgeTest, EveryRuntimeSensorGetsAnMqttComponent) {
  sensor::Sensor *a = fresh_sensor();
  sensor::Sensor *b = fresh_sensor();
  TestConfig &c = this->boot_with({a, b});
  EXPECT_EQ(c.walked(), 6u);
  const auto &children = this->client->children();
  for (mqtt::MQTTComponent *bridge : this->bridged()) {
    EXPECT_STREQ(bridge->component_type(), "sensor");
    EXPECT_FALSE(bridge->is_internal());
    EXPECT_EQ(std::count(children.begin(), children.end(), bridge), 1);
  }
}

// A sensor codegen already serves keeps its own component; a gap is skipped; a sensor two
// sources name gets one bridge.
TEST_F(BridgeTest, OnlySensorsWithoutAComponentAreBridged) {
  sensor::Sensor *runtime = fresh_sensor();
  sensor::Sensor *yaml = fresh_sensor();
  TestConfig &c = this->boot_with({runtime, nullptr, yaml, runtime}, {yaml});
  EXPECT_EQ(c.walked(), 4u + 1u + 1u);
  ASSERT_EQ(this->bridged().size(), 1u);
}

TEST_F(BridgeTest, NoSourceNoBridge) {
  TestConfig &c = this->boot();
  EXPECT_EQ(c.walked(), 4u);
}

// The one test that uses the named, registered sensor: its bridge stays subscribed for good.
TEST_F(BridgeTest, ABridgedSensorPublishesUnderItsOwnName) {
  sensor::Sensor &temp = entities().temp3;
  temp.set_accuracy_decimals(1);
  this->boot_with({&temp});
  this->client->connect_for_test();
  this->settle();
  this->client->published.clear();
  temp.publish_state(21.54f);
  ASSERT_EQ(this->client->published.size(), 1u);
  EXPECT_EQ(this->client->published[0].topic, std::string(NODE_PREFIX) + "/sensor/temp_3/state");
  EXPECT_EQ(this->client->published[0].payload, "21.5");
}

TEST_F(BridgeTest, ABridgedSensorIsCleanedWithTheRest) {
  sensor::Sensor *a = fresh_sensor();
  MqttRecord stored = enabled_record();
  stored.discovery = true;
  this->plant(stored);
  this->boot([a](TestConfig &c) { c.add_runtime_sensors([a]() { return std::vector<sensor::Sensor *>{a}; }); });
  this->connect_and_settle();
  EXPECT_EQ(this->discovery_publishes("{}"), 4u);
  this->client->published.clear();
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  ASSERT_EQ(this->bridged().size(), 1u);
  EXPECT_TRUE(this->bridged()[0]->is_resend_pending());
  this->settle();
  EXPECT_EQ(this->discovery_publishes(""), 4u);
}

}  // namespace esphome::mqtt_config::testing
