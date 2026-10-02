#include "common.h"

namespace esphome::mqtt_config::testing {

// The 200 messages are the dashboard's too (client/device/mqttRules.ts, MQTT_SAVE_MESSAGES).
static const char *const STARTED = "Saved; MQTT is connecting";
static const char *const REBOOT = "Saved; applies after a reboot";
static const char *const SAVED = "Saved";
static const char *const UNCHANGED = "Nothing changed";
static const char *const STORE_FAILED = "Storing the MQTT settings failed; the old ones stay in force";

static MqttPatch turn_on(const char *broker = "10.0.2.2") {
  MqttPatch p;
  p.enabled = true;
  p.broker = std::string(broker);
  return p;
}

class UpdateTest : public MqttTest {};

TEST_F(UpdateTest, TheFirstEnableInABootStartsTheClientAtOnce) {
  TestConfig &c = this->boot();
  MqttPatch patch = turn_on();
  patch.port = 1884;
  patch.username = std::string("jxd");
  patch.password = std::string("pw");
  const auto result = this->save(patch);
  EXPECT_EQ(result.code, 200);
  EXPECT_STREQ(result.message, STARTED);
  EXPECT_TRUE(result.started);
  EXPECT_FALSE(result.reboot_required);
  EXPECT_EQ(result.cleanup, DiscoveryCleanup::NONE);
  EXPECT_EQ(this->client->enable_calls, 1);
  EXPECT_EQ(this->client->credentials().address, "10.0.2.2");
  EXPECT_EQ(this->client->credentials().port, 1884);
  EXPECT_EQ(this->client->credentials().username, "jxd");
  EXPECT_EQ(this->client->credentials().password, "pw");
  EXPECT_FALSE(this->client->is_discovery_enabled());
  EXPECT_TRUE(c.running());
  EXPECT_EQ(c.state(), MqttState::CONNECTING);
  EXPECT_FALSE(c.reboot_required());
  // Stored for the next boot, which starts it the same way.
  EXPECT_TRUE(from_stored(this->board.record()).enabled);
  JsonDocument doc = this->settings_json();
  EXPECT_EQ(doc["running"]["broker"].as<std::string>(), "10.0.2.2");
  EXPECT_FALSE(doc["apply_now"].as<bool>());
}

TEST_F(UpdateTest, AFirstEnableWithDiscoveryAnnouncesWhatWasCompiled) {
  this->boot();
  MqttPatch patch = turn_on();
  patch.discovery = true;
  ASSERT_TRUE(this->save(patch).started);
  EXPECT_EQ(this->client->get_discovery_info().prefix, "homeassistant");
  EXPECT_FALSE(this->client->get_discovery_info().clean);
  this->connect_and_settle();
  EXPECT_EQ(this->discovery_publishes("{}"), 3u);  // Relay 1, Relay 2, Input 1; never Hidden
}

TEST_F(UpdateTest, AFirstEnableWithAClientIdPushesIt) {
  this->boot();
  MqttPatch patch = turn_on();
  patch.client_id = std::string("jxd-kitchen");
  ASSERT_TRUE(this->save(patch).started);
  EXPECT_EQ(this->client->credentials().client_id, "jxd-kitchen");
}

// An earlier "MQTT off" left entries behind; turning MQTT on with discovery off removes them.
TEST_F(UpdateTest, AFirstEnableWithACleanupPendingUsesCleanMode) {
  MqttRecord stored = enabled_record();
  stored.enabled = false;
  stored.clean_pending = true;
  this->plant(stored);
  TestConfig &c = this->boot();
  const auto result = this->save(patch_of([](MqttPatch &p) { p.enabled = true; }));
  EXPECT_STREQ(result.message, STARTED);
  EXPECT_EQ(result.cleanup, DiscoveryCleanup::PENDING);  // not connected yet
  EXPECT_TRUE(this->client->get_discovery_info().clean);
  this->connect_and_settle();
  EXPECT_EQ(this->discovery_publishes(""), 3u);
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
  EXPECT_FALSE(from_stored(this->board.record()).clean_pending);
}

TEST_F(UpdateTest, ALaterChangeWaitsForARebootAndLeavesTheClientAlone) {
  TestConfig &c = this->boot();
  ASSERT_TRUE(this->save(turn_on()).started);
  const auto result = this->save(patch_of([](MqttPatch &p) {
    p.broker = std::string("192.168.1.20");
    p.password = std::string("new");
    p.username = std::string("jxd");
  }));
  EXPECT_EQ(result.code, 200);
  EXPECT_STREQ(result.message, REBOOT);
  EXPECT_FALSE(result.started);
  EXPECT_TRUE(result.reboot_required);
  EXPECT_TRUE(c.reboot_required());
  EXPECT_EQ(this->client->enable_calls, 1);
  EXPECT_EQ(this->client->credentials().address, "10.0.2.2");
  EXPECT_EQ(this->client->credentials().password, "");
  JsonDocument doc = this->settings_json();
  EXPECT_EQ(doc["broker"].as<std::string>(), "192.168.1.20");
  EXPECT_EQ(doc["running"]["broker"].as<std::string>(), "10.0.2.2");
  EXPECT_TRUE(doc["reboot_required"].as<bool>());

  // Putting it back as it runs needs no reboot any more.
  const auto back = this->save(patch_of([](MqttPatch &p) {
    p.broker = std::string("10.0.2.2");
    p.password = std::string("");
    p.username = std::string("");
  }));
  EXPECT_STREQ(back.message, SAVED);
  EXPECT_FALSE(back.reboot_required);
}

TEST_F(UpdateTest, TurningMqttOffWhileRunningWaitsForAReboot) {
  TestConfig &c = this->boot();
  ASSERT_TRUE(this->save(turn_on()).started);
  const auto result = this->save(patch_of([](MqttPatch &p) { p.enabled = false; }));
  EXPECT_STREQ(result.message, REBOOT);
  EXPECT_TRUE(c.reboot_required());
  EXPECT_EQ(this->client->disable_calls, 0);  // disable() would not take the client off the broker
}

// Command topics were subscribed with the boot's prefix.
TEST_F(UpdateTest, AFirstEnableThatChangesThePrefixWaitsForAReboot) {
  TestConfig &c = this->boot();
  MqttPatch patch = turn_on();
  patch.topic_prefix = std::string("home/kitchen");
  const auto result = this->save(patch);
  EXPECT_STREQ(result.message, REBOOT);
  EXPECT_FALSE(result.started);
  EXPECT_TRUE(result.reboot_required);
  EXPECT_FALSE(c.running());
  EXPECT_EQ(this->client->enable_calls, 0);
  EXPECT_EQ(c.state(), MqttState::OFF);
  EXPECT_TRUE(this->settings_json()["apply_now"].as<bool>());

  // The prefix back to the boot's: now it starts.
  const auto again = this->save(patch_of([](MqttPatch &p) { p.topic_prefix = std::string(""); }));
  EXPECT_STREQ(again.message, STARTED);
  EXPECT_FALSE(c.reboot_required());
}

TEST_F(UpdateTest, APrefixOrClientIdEqualToTheDefaultNeedsNoReboot) {
  TestConfig &c = this->boot();
  MqttPatch patch = turn_on();
  patch.topic_prefix = std::string(NODE_PREFIX);  // the default, spelled out
  ASSERT_TRUE(this->save(patch).started);
  const std::string default_id = this->settings_json()["client_id_default"].as<std::string>();
  const auto result = this->save(patch_of([&default_id](MqttPatch &p) { p.client_id = default_id; }));
  EXPECT_STREQ(result.message, SAVED);
  EXPECT_FALSE(c.reboot_required());
  const auto cleared = this->save(patch_of([](MqttPatch &p) { p.topic_prefix = std::string(""); }));
  EXPECT_STREQ(cleared.message, SAVED);
  EXPECT_FALSE(c.reboot_required());
}

TEST_F(UpdateTest, AChangeWhileOffIsSimplySaved) {
  this->boot();
  const auto result = this->save(patch_of([](MqttPatch &p) {
    p.broker = std::string("broker");
    p.port = 1884;
  }));
  EXPECT_EQ(result.code, 200);
  EXPECT_STREQ(result.message, SAVED);
  EXPECT_FALSE(result.reboot_required);
  EXPECT_EQ(this->config->state(), MqttState::OFF);
}

TEST_F(UpdateTest, NothingNewIsNothingChangedAndNothingIsWritten) {
  MqttRecord stored = enabled_record();
  this->plant(stored);
  TestConfig &c = this->boot();
  const auto result = this->save(patch_of([](MqttPatch &p) {
    p.enabled = true;
    p.broker = std::string("192.168.1.10");
  }));
  EXPECT_EQ(result.code, 200);
  EXPECT_STREQ(result.message, UNCHANGED);
  EXPECT_EQ(c.stores, 0);
}

TEST_F(UpdateTest, ABrokenRuleIsA400AndNothingIsStored) {
  TestConfig &c = this->boot();
  const auto result = this->save(turn_on("mqtt://10.0.2.2"));
  EXPECT_EQ(result.code, 400);
  EXPECT_STREQ(result.message, "'broker' takes a host name, not a URL: drop the 'mqtt://'");
  EXPECT_EQ(c.stores, 0);
  EXPECT_FALSE(c.running());
  const auto port = this->save(patch_of([](MqttPatch &p) { p.port = 70000; }));
  EXPECT_EQ(port.code, 400);
  EXPECT_STREQ(port.message, "'port' must be between 1 and 65535");
}

// The flush failed and the record is not in NVS: the device stays as it was.
TEST_F(UpdateTest, AFailedStoreIsA500AndChangesNothing) {
  TestConfig &c = this->boot();
  c.flush = TestConfig::Flush::FAILED_RECORD_LOST;
  const auto result = this->save(turn_on());
  EXPECT_EQ(result.code, 500);
  EXPECT_STREQ(result.message, STORE_FAILED);
  EXPECT_FALSE(result.started);
  EXPECT_FALSE(c.running());
  EXPECT_EQ(this->client->enable_calls, 0);
  EXPECT_TRUE(this->board.nvs.empty());
  EXPECT_FALSE(this->settings_json()["enabled"].as<bool>());
}

// The flush reports for every key at once; this record reached NVS all the same.
TEST_F(UpdateTest, AFlushThatFailedForAnotherRecordStillCountsAsStored) {
  TestConfig &c = this->boot();
  c.flush = TestConfig::Flush::FAILED_RECORD_LANDED;
  const auto result = this->save(turn_on());
  EXPECT_EQ(result.code, 200);
  EXPECT_STREQ(result.message, STARTED);
  EXPECT_TRUE(from_stored(this->board.record()).enabled);
}

TEST_F(UpdateTest, AHeldBackClientIsNotStartedByASave) {
  this->board.rtc = CrashGuardRecord{CRASH_GUARD_MAGIC, 2, 1, {0, 0}};
  this->board.panic = true;
  TestConfig &c = this->boot();
  const auto result = this->save(turn_on());
  EXPECT_STREQ(result.message, REBOOT);
  EXPECT_FALSE(c.running());
  EXPECT_TRUE(c.reboot_required());
  EXPECT_EQ(this->client->enable_calls, 0);
}

// With the broker away the old one keeps the entries until it is back; MQTT stays on, so that
// is a while at most.
TEST_F(UpdateTest, ABrokerChangeWhileDisconnectedSimplyWaitsForTheReboot) {
  MqttRecord stored = enabled_record();
  stored.discovery = true;
  this->plant(stored);
  this->boot();
  const auto result = this->save(patch_of([](MqttPatch &p) { p.broker = std::string("192.168.1.20"); }));
  EXPECT_STREQ(result.message, REBOOT);
  EXPECT_TRUE(result.reboot_required);
  EXPECT_EQ(result.cleanup, DiscoveryCleanup::PENDING);
}

// A soft reset clears the streak, so the reboot the notice offers is what retries it.
TEST_F(UpdateTest, TheRebootAfterASaveAppliesIt) {
  TestConfig &first = this->boot();
  ASSERT_TRUE(this->save(turn_on()).started);
  this->save(patch_of([](MqttPatch &p) { p.topic_prefix = std::string("home/kitchen"); }));
  ASSERT_TRUE(first.reboot_required());
  TestConfig &second = this->reboot();
  EXPECT_TRUE(second.running());
  EXPECT_FALSE(second.reboot_required());
  EXPECT_EQ(this->client->get_topic_prefix(), "home/kitchen");
  EXPECT_EQ(this->client->credentials().address, "10.0.2.2");
}

}  // namespace esphome::mqtt_config::testing

namespace esphome::mqtt_config::testing {

// The preference itself, not the seam: the host backend refuses a record over 255 bytes,
// which is what a flash that will not take the write looks like from here.
class RealStore : public MqttConfig {
 public:
  RealStore(mqtt::MQTTClientComponent *client, Board *board) : MqttConfig(client), board_(board) {}

 protected:
  CrashGuardRecord &guard_record_() override { return this->board_->rtc; }
  void add_log_listener_() override {}
  Board *board_;
};

TEST(MqttConfigRealStore, ARecordTheBackendRefusesIsA500) {
  name_the_node();
  host::setup_preferences();
  Board board;
  auto client = std::make_unique<mqtt::MQTTClientComponent>();
  RealStore config(client.get(), &board);
  config.set_preference_hash(PREF_HASH);
  config.setup();
  EXPECT_EQ(config.state(), MqttState::NOT_CONFIGURED);
  const auto result = config.update(turn_on());
  EXPECT_EQ(result.code, 500);
  EXPECT_STREQ(result.message, STORE_FAILED);
  EXPECT_FALSE(config.running());
}

}  // namespace esphome::mqtt_config::testing
