#include "common.h"

// What the display menu's MQTT rows read.
namespace esphome::mqtt_config::testing {

using Reason = mqtt::MQTTClientDisconnectReason;

class PanelTest : public MqttTest {};

TEST_F(PanelTest, TheStateRowFollowsEveryState) {
  TestConfig &c = this->boot();
  EXPECT_STREQ(c.panel_state_text(), "MQTT: Not set");
  this->save(patch_of([](MqttPatch &p) { p.broker = std::string("broker"); }));
  EXPECT_STREQ(c.panel_state_text(), "MQTT: Off");
  this->save(patch_of([](MqttPatch &p) { p.enabled = true; }));
  EXPECT_STREQ(c.panel_state_text(), "MQTT: Connecting");
  this->client->connect_for_test();
  EXPECT_STREQ(c.panel_state_text(), "MQTT: Connected");
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_STREQ(c.panel_state_text(), "MQTT: Disconnected");
}

TEST_F(PanelTest, AHeldBackClientSaysSo) {
  this->plant(enabled_record());
  this->board.rtc = CrashGuardRecord{CRASH_GUARD_MAGIC, 2, 1, {0, 0}};
  this->board.panic = true;
  TestConfig &c = this->boot();
  ASSERT_EQ(c.state(), MqttState::OFF);
  EXPECT_STREQ(c.panel_state_text(), "MQTT: Held back");
}

TEST_F(PanelTest, NoBrokerIsEmpty) {
  TestConfig &c = this->boot();
  EXPECT_EQ(c.panel_broker(), "");
}

TEST_F(PanelTest, TheDefaultPortIsLeftOut) {
  MqttRecord stored = enabled_record("10.0.2.2");
  stored.enabled = false;
  this->plant(stored);
  TestConfig &c = this->boot();
  EXPECT_FALSE(c.running());
  EXPECT_EQ(c.panel_broker(), "10.0.2.2");
}

TEST_F(PanelTest, AnotherPortIsShown) {
  MqttRecord stored = enabled_record("mqtt.office.example.com");
  stored.port = 1884;
  this->plant(stored);
  TestConfig &c = this->boot();
  EXPECT_EQ(c.panel_broker(), "mqtt.office.example.com:1884");
}

TEST_F(PanelTest, AnIpv6AddressIsBracketedBeforeItsPort) {
  MqttRecord stored = enabled_record("fd00::1");
  stored.port = 8883;
  this->plant(stored);
  TestConfig &c = this->boot();
  EXPECT_EQ(c.panel_broker(), "[fd00::1]:8883");
}

// A saved change waits for the reboot, and the row says what runs until then.
TEST_F(PanelTest, TheRunningBrokerIsShownOverASavedOne) {
  this->plant(enabled_record("10.0.2.2"));
  TestConfig &c = this->boot();
  this->save(patch_of([](MqttPatch &p) {
    p.broker = std::string("192.168.1.20");
    p.port = 1884;
  }));
  ASSERT_TRUE(c.reboot_required());
  EXPECT_EQ(c.panel_broker(), "10.0.2.2");
}

TEST_F(PanelTest, WhileOffTheStoredBrokerIsShown) {
  TestConfig &c = this->boot();
  this->save(patch_of([](MqttPatch &p) { p.broker = std::string("broker.lan"); }));
  EXPECT_EQ(c.panel_broker(), "broker.lan");
  this->save(patch_of([](MqttPatch &p) { p.enabled = true; }));
  EXPECT_TRUE(c.running());
  EXPECT_EQ(c.panel_broker(), "broker.lan");
}

TEST_F(PanelTest, DiscoveryWhileOffIsTheStoredSetting) {
  MqttRecord stored;
  stored.broker = "10.0.2.2";
  stored.discovery = true;
  this->plant(stored);
  TestConfig &c = this->boot();
  EXPECT_FALSE(c.running());
  EXPECT_TRUE(c.panel_discovery());
}

// Off applies at once; on waits for the reboot.
TEST_F(PanelTest, DiscoveryWhileRunningIsWhatRuns) {
  MqttRecord stored = enabled_record();
  stored.discovery = true;
  this->plant(stored);
  TestConfig &c = this->boot();
  EXPECT_TRUE(c.panel_discovery());
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  EXPECT_FALSE(c.panel_discovery());
  this->save(patch_of([](MqttPatch &p) { p.discovery = true; }));
  EXPECT_TRUE(c.reboot_required());
  EXPECT_FALSE(c.panel_discovery());
}

}  // namespace esphome::mqtt_config::testing
