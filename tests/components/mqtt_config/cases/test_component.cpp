#include "common.h"

namespace esphome::mqtt_config::testing {

// Below BEFORE_CONNECTION, above the client and the entity components it configures, and
// below dallas_scan, whose sensors it bridges.
TEST(MqttConfigComponent, SetsUpBetweenTheClientAndTheConnection) {
  mqtt::MQTTClientComponent client;
  MqttConfig config(&client);
  const float priority = config.get_setup_priority();
  EXPECT_GT(priority, client.get_setup_priority());
  EXPECT_LT(priority, setup_priority::BEFORE_CONNECTION);
  EXPECT_LT(priority, setup_priority::DATA);
  EXPECT_FLOAT_EQ(priority, 210.0f);
}

TEST(MqttConfigComponent, TheConstructorIsTheGlobalOne) {
  mqtt::MQTTClientComponent client;
  MqttConfig config(&client);
  EXPECT_EQ(global_mqtt_config, &config);
  EXPECT_EQ(config.client(), &client);
}

// The devices build without IPv6, and so does this suite, the way network defines it.
TEST(MqttConfigComponent, ThisBuildChecksBrokersByTheIpv4Rules) { EXPECT_FALSE(BUILD_HAS_IPV6); }

TEST(MqttConfigComponent, KeysAndLabelsAreTheContractsWords) {
  EXPECT_STREQ(MqttConfig::state_key(MqttState::NOT_CONFIGURED), "not_configured");
  EXPECT_STREQ(MqttConfig::state_key(MqttState::OFF), "off");
  EXPECT_STREQ(MqttConfig::state_key(MqttState::CONNECTING), "connecting");
  EXPECT_STREQ(MqttConfig::state_key(MqttState::CONNECTED), "connected");
  EXPECT_STREQ(MqttConfig::state_key(MqttState::DISCONNECTED), "disconnected");

  const std::vector<std::tuple<MqttError, const char *, const char *>> errors = {
      {MqttError::DNS, "dns", "Broker name not found"},
      {MqttError::UNREACHABLE, "unreachable", "Broker unreachable"},
      {MqttError::CONNECTION_LOST, "connection_lost", "Connection lost"},
      {MqttError::PROTOCOL, "protocol", "Protocol version refused"},
      {MqttError::IDENTIFIER_REJECTED, "identifier_rejected", "Client ID refused"},
      {MqttError::SERVER_UNAVAILABLE, "server_unavailable", "Broker unavailable"},
      {MqttError::BAD_CREDENTIALS, "bad_credentials", "Wrong username or password"},
      {MqttError::NOT_AUTHORIZED, "not_authorized", "Wrong username or password"},
      {MqttError::CRASH_GUARD, "crash_guard", "Held back after repeated crashes"},
  };
  for (const auto &[error, key, label] : errors) {
    EXPECT_STREQ(MqttConfig::error_key(error), key);
    EXPECT_STREQ(MqttConfig::error_label(error), label);
  }
  EXPECT_EQ(MqttConfig::error_key(MqttError::NONE), nullptr);
  EXPECT_STREQ(MqttConfig::error_label(MqttError::NONE), "");

  EXPECT_STREQ(MqttConfig::cleanup_key(DiscoveryCleanup::NONE), "none");
  EXPECT_STREQ(MqttConfig::cleanup_key(DiscoveryCleanup::RUNNING), "running");
  EXPECT_STREQ(MqttConfig::cleanup_key(DiscoveryCleanup::PENDING), "pending");
}

// A value no enumerator names still reads as something the contract knows.
TEST(MqttConfigComponent, AnUnknownValueReadsAsTheQuietestKey) {
  EXPECT_STREQ(MqttConfig::state_key(static_cast<MqttState>(99)), "off");
  EXPECT_EQ(MqttConfig::error_key(static_cast<MqttError>(99)), nullptr);
  EXPECT_STREQ(MqttConfig::error_label(static_cast<MqttError>(99)), "");
  EXPECT_STREQ(MqttConfig::cleanup_key(static_cast<DiscoveryCleanup>(99)), "none");
}

class ComponentTest : public MqttTest {};

TEST_F(ComponentTest, DumpConfigSaysAPasswordIsSetAndNeverWhatItIs) {
  MqttRecord stored = enabled_record();
  stored.username = "jxd";
  stored.password = "s3cret-phrase";
  this->plant(stored);
  TestConfig &c = this->boot();
  LogCapture::instance().clear();
  c.dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Password: set"));
  EXPECT_FALSE(LogCapture::instance().has("s3cret-phrase"));
}

TEST_F(ComponentTest, DumpConfigSaysWhenNoPasswordIsSet) {
  TestConfig &c = this->boot();
  LogCapture::instance().clear();
  c.dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Password: not set"));
}

TEST_F(ComponentTest, DumpConfigNamesANoticeAndAHeldBackClient) {
  MqttRecord stored = enabled_record("bad host");
  this->plant(stored);
  this->board.rtc = CrashGuardRecord{CRASH_GUARD_MAGIC, 2, 1, {0, 0}};
  this->board.panic = true;
  TestConfig &c = this->boot();
  LogCapture::instance().clear();
  c.dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Stored notice: invalid: 'broker' must be a host name or an IPv4 address"));
  EXPECT_TRUE(LogCapture::instance().has("Held back after 3 crashes in a row"));
}

TEST_F(ComponentTest, TheSettingsAnswerNeverCarriesThePassword) {
  MqttRecord stored = enabled_record();
  stored.username = "jxd";
  stored.password = "s3cret-phrase";
  this->plant(stored);
  this->boot();
  JsonDocument doc = this->settings_json();
  EXPECT_FALSE(doc["password"].is<const char *>());
  EXPECT_TRUE(doc["password_set"].as<bool>());
  std::string text;
  serializeJson(doc, text);
  EXPECT_EQ(text.find("s3cret-phrase"), std::string::npos);
}

TEST_F(ComponentTest, TheSettingsAnswerOfAFreshDevice) {
  this->boot();
  JsonDocument doc = this->settings_json();
  EXPECT_FALSE(doc["enabled"].as<bool>());
  EXPECT_EQ(doc["broker"].as<std::string>(), "");
  EXPECT_EQ(doc["port"].as<int>(), 1883);
  EXPECT_EQ(doc["username"].as<std::string>(), "");
  EXPECT_FALSE(doc["password_set"].as<bool>());
  EXPECT_EQ(doc["client_id"].as<std::string>(), "");
  EXPECT_EQ(doc["client_id_default"].as<std::string>(), this->client->credentials().client_id);
  EXPECT_EQ(doc["client_id_default"].as<std::string>().rfind(std::string(NODE) + "-", 0), 0u);
  EXPECT_EQ(doc["topic_prefix"].as<std::string>(), "");
  EXPECT_EQ(doc["topic_prefix_default"].as<std::string>(), NODE_PREFIX);
  EXPECT_FALSE(doc["discovery"].as<bool>());
  EXPECT_EQ(doc["state"].as<std::string>(), "not_configured");
  EXPECT_TRUE(doc["last_error"].isNull());
  EXPECT_TRUE(doc["running"].isNull());
  EXPECT_TRUE(doc["apply_now"].as<bool>());
  EXPECT_FALSE(doc["reboot_required"].as<bool>());
  EXPECT_EQ(doc["discovery_cleanup"].as<std::string>(), "none");
  EXPECT_TRUE(doc["stored_notice"].isNull());
}

TEST_F(ComponentTest, TheSettingsAnswerOfARunningClient) {
  MqttRecord stored = enabled_record();
  stored.port = 1884;
  stored.username = "jxd";
  stored.password = "pw";
  stored.discovery = true;
  this->plant(stored);
  this->boot();
  this->client->connect_for_test();
  JsonDocument doc = this->settings_json();
  EXPECT_TRUE(doc["enabled"].as<bool>());
  EXPECT_EQ(doc["broker"].as<std::string>(), "192.168.1.10");
  EXPECT_EQ(doc["port"].as<int>(), 1884);
  EXPECT_EQ(doc["username"].as<std::string>(), "jxd");
  EXPECT_TRUE(doc["discovery"].as<bool>());
  EXPECT_EQ(doc["state"].as<std::string>(), "connected");
  JsonObject running = doc["running"];
  ASSERT_FALSE(running.isNull());
  EXPECT_EQ(running["broker"].as<std::string>(), "192.168.1.10");
  EXPECT_EQ(running["port"].as<int>(), 1884);
  EXPECT_EQ(running["client_id"].as<std::string>(), doc["client_id_default"].as<std::string>());
  EXPECT_EQ(running["topic_prefix"].as<std::string>(), NODE_PREFIX);
  EXPECT_EQ(running["status_topic"].as<std::string>(), std::string(NODE_PREFIX) + "/status");
  EXPECT_TRUE(running["discovery"].as<bool>());
  EXPECT_FALSE(doc["apply_now"].as<bool>());
}

TEST_F(ComponentTest, TheSettingsAnswerShowsAStoredNotice) {
  StoredMqttV1 foreign = record_of(enabled_record());
  foreign.layout = 2;
  foreign.min_reader = 2;
  this->board.write(foreign);
  this->boot();
  JsonDocument doc = this->settings_json();
  EXPECT_EQ(doc["stored_notice"].as<std::string>(), "newer_firmware");
  EXPECT_FALSE(doc["enabled"].as<bool>());
  EXPECT_EQ(doc["broker"].as<std::string>(), "");
}

}  // namespace esphome::mqtt_config::testing
