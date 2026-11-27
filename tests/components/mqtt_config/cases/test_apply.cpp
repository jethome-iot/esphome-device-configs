#include "common.h"

namespace esphome::mqtt_config::testing {

class ApplyTest : public MqttTest {};

static const std::string STATUS_TOPIC = std::string(NODE_PREFIX) + "/status";

// Codegen compiles `<node>/status`; each device's own prefix carries the MAC.
TEST_F(ApplyTest, NothingStoredRetargetsTheStatusTopicsAndLeavesTheClientIdle) {
  TestConfig &c = this->boot();
  EXPECT_EQ(this->client->birth_message().topic, STATUS_TOPIC);
  EXPECT_EQ(this->client->birth_message().payload, "online");
  EXPECT_TRUE(this->client->birth_message().retain);
  EXPECT_EQ(this->client->last_will().topic, STATUS_TOPIC);
  EXPECT_EQ(this->client->last_will().payload, "offline");
  EXPECT_EQ(this->client->shutdown_message().topic, STATUS_TOPIC);
  EXPECT_EQ(this->client->shutdown_message().payload, "offline");
  EXPECT_EQ(this->client->get_topic_prefix(), NODE_PREFIX);

  EXPECT_FALSE(this->client->is_discovery_enabled());
  EXPECT_EQ(this->client->credentials().address, "");
  EXPECT_FALSE(this->client->enable_on_boot());
  EXPECT_EQ(this->client->enable_calls, 0);
  EXPECT_FALSE(c.running());
  EXPECT_EQ(c.state(), MqttState::NOT_CONFIGURED);
  EXPECT_FALSE(c.reboot_required());
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
  EXPECT_EQ(c.stores, 0);
}

TEST_F(ApplyTest, AStoredBrokerWithMqttOffIsOff) {
  MqttRecord stored = enabled_record();
  stored.enabled = false;
  this->plant(stored);
  TestConfig &c = this->boot();
  EXPECT_EQ(c.state(), MqttState::OFF);
  EXPECT_FALSE(c.running());
  EXPECT_EQ(this->client->enable_calls, 0);
}

TEST_F(ApplyTest, AnEnabledRecordPushesTheSettingsOnceAndStartsTheClient) {
  MqttRecord stored = enabled_record("broker.local");
  stored.port = 1884;
  stored.username = "jxd";
  stored.password = "pw";
  this->plant(stored);
  const std::string default_id = compiled_client()->credentials().client_id;
  TestConfig &c = this->boot();
  EXPECT_EQ(this->client->credentials().address, "broker.local");
  EXPECT_EQ(this->client->credentials().port, 1884);
  EXPECT_EQ(this->client->credentials().username, "jxd");
  EXPECT_EQ(this->client->credentials().password, "pw");
  EXPECT_EQ(this->client->credentials().client_id, default_id);  // empty keeps the client's
  EXPECT_TRUE(this->client->enable_on_boot());
  EXPECT_EQ(this->client->enable_calls, 1);  // by the client's own setup
  EXPECT_TRUE(c.running());
  EXPECT_EQ(c.state(), MqttState::CONNECTING);
  EXPECT_FALSE(this->client->is_discovery_enabled());  // discovery is off by default
  EXPECT_FALSE(c.reboot_required());
}

TEST_F(ApplyTest, AStoredClientIdAndPrefixAreTakenLiterally) {
  MqttRecord stored = enabled_record();
  stored.client_id = "jxd-kitchen";
  stored.topic_prefix = "home/kitchen";
  this->plant(stored);
  this->boot();
  EXPECT_EQ(this->client->credentials().client_id, "jxd-kitchen");
  EXPECT_EQ(this->client->get_topic_prefix(), "home/kitchen");
  EXPECT_EQ(this->client->birth_message().topic, "home/kitchen/status");
  EXPECT_EQ(this->client->last_will().topic, "home/kitchen/status");
  // The entity topics follow, since they are built from the prefix.
  EXPECT_EQ(this->relay1->state_topic(), "home/kitchen/switch/relay_1/state");
}

// The prefix applies with MQTT off too: a first start later in the boot keeps it.
TEST_F(ApplyTest, AStoredPrefixAppliesWithMqttOff) {
  MqttRecord stored;
  stored.topic_prefix = "home/kitchen";
  this->plant(stored);
  TestConfig &c = this->boot();
  EXPECT_EQ(this->client->get_topic_prefix(), "home/kitchen");
  EXPECT_EQ(this->client->birth_message().topic, "home/kitchen/status");
  EXPECT_FALSE(c.running());
}

TEST_F(ApplyTest, DiscoveryOnKeepsWhatWasCompiled) {
  MqttRecord stored = enabled_record();
  stored.discovery = true;
  stored.clean_pending = true;  // the configs are announced again, so nothing is left to clean
  this->plant(stored);
  TestConfig &c = this->boot();
  const mqtt::MQTTDiscoveryInfo &info = this->client->get_discovery_info();
  EXPECT_EQ(info.prefix, "homeassistant");
  EXPECT_FALSE(info.clean);
  EXPECT_TRUE(info.retain);
  EXPECT_FALSE(info.discover_ip);
  EXPECT_EQ(info.unique_id_generator, mqtt::MQTT_MAC_ADDRESS_UNIQUE_ID_GENERATOR);
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
  EXPECT_EQ(c.stores, 0);  // cleared on the next write, not now
}

// The fallback for a cleanup an earlier boot did not finish.
TEST_F(ApplyTest, CleanPendingWithDiscoveryOffBootsInCleanMode) {
  MqttRecord stored = enabled_record();
  stored.clean_pending = true;
  this->plant(stored);
  TestConfig &c = this->boot();
  const mqtt::MQTTDiscoveryInfo &info = this->client->get_discovery_info();
  EXPECT_EQ(info.prefix, "homeassistant");
  EXPECT_TRUE(info.clean);
  EXPECT_TRUE(c.running());
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::PENDING);  // until it connects
  this->client->connect_for_test();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::RUNNING);
}

TEST_F(ApplyTest, CleanPendingWithMqttOffWaitsWithoutConnecting) {
  MqttRecord stored = enabled_record();
  stored.enabled = false;
  stored.clean_pending = true;
  this->plant(stored);
  TestConfig &c = this->boot();
  EXPECT_FALSE(c.running());
  EXPECT_EQ(this->client->enable_calls, 0);
  EXPECT_FALSE(this->client->is_discovery_enabled());
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::PENDING);
  EXPECT_EQ(this->settings_json()["discovery_cleanup"].as<std::string>(), "pending");
}

// Only older firmware with looser rules can have stored it; turning MQTT off would cut the
// device off its broker after an update.
TEST_F(ApplyTest, ARecordThatBreaksARuleIsAppliedWithANotice) {
  MqttRecord stored = enabled_record("my broker");
  this->plant(stored);
  TestConfig &c = this->boot();
  EXPECT_TRUE(c.running());
  EXPECT_EQ(this->client->credentials().address, "my broker");
  EXPECT_EQ(this->settings_json()["stored_notice"].as<std::string>(),
            "invalid: 'broker' must be a host name or an IPv4 address");
  EXPECT_EQ(c.stores, 0);

  // The next save has to pass the rules, and clears the notice.
  EXPECT_EQ(this->save(patch_of([](MqttPatch &p) { p.port = 1884; })).code, 400);
  EXPECT_EQ(this->save(patch_of([](MqttPatch &p) { p.broker = std::string("broker"); })).code, 200);
  EXPECT_TRUE(this->settings_json()["stored_notice"].isNull());
}

TEST_F(ApplyTest, AnEnabledRecordWithoutABrokerDoesNotStart) {
  MqttRecord stored = enabled_record("");
  this->plant(stored);
  TestConfig &c = this->boot();
  EXPECT_FALSE(c.running());
  EXPECT_EQ(this->client->enable_calls, 0);
  EXPECT_EQ(this->settings_json()["stored_notice"].as<std::string>(), "invalid: 'broker' is required to turn MQTT on");
}

TEST_F(ApplyTest, NewerFirmwaresRecordLeavesMqttOff) {
  StoredMqttV1 planted = record_of(enabled_record());
  planted.min_reader = 2;
  this->board.write(planted);
  TestConfig &c = this->boot();
  EXPECT_FALSE(c.running());
  EXPECT_FALSE(this->client->is_discovery_enabled());
  EXPECT_EQ(this->settings_json()["stored_notice"].as<std::string>(), "newer_firmware");
}

TEST_F(ApplyTest, AHeldBackClientDoesNotStartAndARestartIsDue) {
  MqttRecord stored = enabled_record();
  stored.discovery = true;
  this->plant(stored);
  this->board.rtc = CrashGuardRecord{CRASH_GUARD_MAGIC, 2, 1, {0, 0}};
  this->board.panic = true;
  TestConfig &c = this->boot();
  EXPECT_EQ(c.crash_streak(), 3);
  EXPECT_FALSE(c.running());
  EXPECT_EQ(this->client->enable_calls, 0);
  EXPECT_FALSE(this->client->is_discovery_enabled());
  EXPECT_EQ(c.state(), MqttState::OFF);
  EXPECT_EQ(c.last_error(), MqttError::CRASH_GUARD);
  EXPECT_TRUE(c.reboot_required());
  JsonDocument doc = this->settings_json();
  EXPECT_FALSE(doc["apply_now"].as<bool>());
  EXPECT_EQ(doc["last_error"].as<std::string>(), "crash_guard");
}

TEST_F(ApplyTest, TheCallbacksAndTheLogListenerAreRegistered) {
  TestConfig &c = this->boot();
  EXPECT_TRUE(this->board.log_listener_added);
  EXPECT_EQ(this->client->on_connect_callbacks(), 1u);
  EXPECT_EQ(this->client->on_disconnect_callbacks(), 1u);
  // Each one is this component's: a connect and a drop reach its state.
  this->client->connect_for_test();
  EXPECT_TRUE(c.connected());
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::DNS_RESOLVE_ERROR);
  EXPECT_FALSE(c.connected());
  EXPECT_EQ(c.last_error(), MqttError::DNS);
  EXPECT_TRUE(LogCapture::instance().has("Not connected: Broker name not found"));
}

// The client's own setup at 200 and the entities' at 100 come after this one at 210.
TEST_F(ApplyTest, HiddenEntitiesStayOffTheClient) {
  this->boot();
  EXPECT_TRUE(this->hidden->is_internal());
  EXPECT_FALSE(this->relay1->is_internal());
  const auto &children = this->client->children();
  EXPECT_EQ(std::count(children.begin(), children.end(), this->hidden.get()), 0);
  EXPECT_EQ(std::count(children.begin(), children.end(), this->relay1.get()), 1);
  EXPECT_EQ(this->config->walked(), 4u);
}

}  // namespace esphome::mqtt_config::testing
