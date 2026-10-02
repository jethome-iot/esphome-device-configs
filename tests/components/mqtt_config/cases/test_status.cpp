#include <functional>
#include <set>
#include "common.h"

namespace esphome::mqtt_config::testing {

using Reason = mqtt::MQTTClientDisconnectReason;

class StatusTest : public MqttTest {
 protected:
  TestConfig &running() {
    this->plant(enabled_record());
    return this->boot();
  }
  // The line upstream's ESP32 backend logs under its tag just before the drop.
  void backend_logs(const std::string &line) { this->config->log(ESPHOME_LOG_LEVEL_ERROR, "mqtt", line); }
};

TEST_F(StatusTest, TheStatesInOrder) {
  TestConfig &c = this->boot();
  EXPECT_EQ(c.state(), MqttState::NOT_CONFIGURED);
  this->save(patch_of([](MqttPatch &p) { p.broker = std::string("broker"); }));
  EXPECT_EQ(c.state(), MqttState::OFF);
  this->save(patch_of([](MqttPatch &p) { p.enabled = true; }));
  EXPECT_EQ(c.state(), MqttState::CONNECTING);
  EXPECT_TRUE(c.running());
  this->client->connect_for_test();
  EXPECT_EQ(c.state(), MqttState::CONNECTED);
  EXPECT_TRUE(c.connected());
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.state(), MqttState::DISCONNECTED);
  EXPECT_FALSE(c.connected());
  EXPECT_EQ(c.last_error(), MqttError::CONNECTION_LOST);
  this->client->connect_for_test();
  EXPECT_EQ(c.state(), MqttState::CONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::NONE);
}

TEST_F(StatusTest, ABrokerNameThatDoesNotResolve) {
  TestConfig &c = this->running();
  this->client->drop_for_test(Reason::DNS_RESOLVE_ERROR);
  EXPECT_EQ(c.last_error(), MqttError::DNS);
  EXPECT_EQ(c.state(), MqttState::DISCONNECTED);
}

TEST_F(StatusTest, ADropBeforeAnyConnectionIsUnreachable) {
  TestConfig &c = this->running();
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::UNREACHABLE);
}

TEST_F(StatusTest, ASocketErrorLineIsUnreachable) {
  TestConfig &c = this->running();
  this->backend_logs("Last esp-tls error: 0x0, tls stack error: 0x0, socket errno: 111 (Connection refused)");
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::UNREACHABLE);
}

// esp-mqtt's connect return codes 1 to 5.
TEST_F(StatusTest, ARefusedConnectionSaysWhy) {
  const std::vector<std::pair<const char *, MqttError>> codes = {
      {"0x1", MqttError::PROTOCOL},           {"0x2", MqttError::IDENTIFIER_REJECTED},
      {"0x3", MqttError::SERVER_UNAVAILABLE}, {"0x4", MqttError::BAD_CREDENTIALS},
      {"0x5", MqttError::NOT_AUTHORIZED},
  };
  TestConfig &c = this->running();
  for (const auto &[code, error] : codes) {
    this->backend_logs(std::string("Connection refused error: ") + code);
    this->client->drop_for_test(Reason::TCP_DISCONNECTED);
    EXPECT_EQ(c.last_error(), error) << code;
    EXPECT_EQ(c.state(), MqttState::DISCONNECTED);
  }
}

// A cause is read once: the next drop without a line of its own is unreachable again.
TEST_F(StatusTest, ALoggedCauseCountsForTheNextDropOnly) {
  TestConfig &c = this->running();
  this->backend_logs("Connection refused error: 0x5");
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::NOT_AUTHORIZED);
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::UNREACHABLE);
}

TEST_F(StatusTest, UnknownCodesAndOtherLinesAreIgnored) {
  TestConfig &c = this->running();
  for (const char *line :
       {"Connection refused error: 0x6", "Connection refused error: 0x", "Connection refused error: 0xzz",
        "Connection refused error: 0x123456", "MQTT_EVENT_ERROR", "Unknown error type: 0x5"}) {
    this->backend_logs(line);
    this->client->drop_for_test(Reason::TCP_DISCONNECTED);
    EXPECT_EQ(c.last_error(), MqttError::UNREACHABLE) << line;
  }
}

TEST_F(StatusTest, OnlyTheClientsErrorsCount) {
  TestConfig &c = this->running();
  c.log(ESPHOME_LOG_LEVEL_WARN, "mqtt", "Connection refused error: 0x5");
  c.log(ESPHOME_LOG_LEVEL_ERROR, "mqtt.component", "Connection refused error: 0x5");
  c.log(ESPHOME_LOG_LEVEL_ERROR, nullptr, "Connection refused error: 0x5");
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::UNREACHABLE);
  c.log(ESPHOME_LOG_LEVEL_ERROR, "mqtt", "Connection refused error: 0X4");  // not upstream's spelling
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::UNREACHABLE);
  for (const char *code : {"0xA", "0xb", "0xf0"}) {  // codes esp-mqtt does not send
    c.log(ESPHOME_LOG_LEVEL_ERROR, "mqtt", std::string("Connection refused error: ") + code);
    this->client->drop_for_test(Reason::TCP_DISCONNECTED);
    EXPECT_EQ(c.last_error(), MqttError::UNREACHABLE) << code;
  }
  c.log(ESPHOME_LOG_LEVEL_ERROR, "mqtt", "Connection refused error: 0x05 (refused)");
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::NOT_AUTHORIZED);
}

// A backend that reports the refusal in the reason itself, as the others than ESP32's do.
TEST_F(StatusTest, ARefusalInTheDropReasonSaysWhyToo) {
  const std::vector<std::pair<Reason, MqttError>> reasons = {
      {Reason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION, MqttError::PROTOCOL},
      {Reason::MQTT_IDENTIFIER_REJECTED, MqttError::IDENTIFIER_REJECTED},
      {Reason::MQTT_SERVER_UNAVAILABLE, MqttError::SERVER_UNAVAILABLE},
      {Reason::MQTT_MALFORMED_CREDENTIALS, MqttError::BAD_CREDENTIALS},
      {Reason::MQTT_NOT_AUTHORIZED, MqttError::NOT_AUTHORIZED},
      {Reason::TLS_BAD_FINGERPRINT, MqttError::UNREACHABLE},
  };
  TestConfig &c = this->running();
  for (const auto &[reason, error] : reasons) {
    this->client->drop_for_test(reason);
    EXPECT_EQ(c.last_error(), error) << static_cast<int>(reason);
  }
}

TEST_F(StatusTest, ADropAfterAConnectionIsALostConnection) {
  TestConfig &c = this->running();
  this->client->connect_for_test();
  this->backend_logs("Last esp-tls error: 0x0, tls stack error: 0x0, socket errno: 104 (Connection reset by peer)");
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::CONNECTION_LOST);
  // The retries after it fail before any connection is up.
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::UNREACHABLE);
}

TEST_F(StatusTest, AConnectClearsTheError) {
  TestConfig &c = this->running();
  this->backend_logs("Connection refused error: 0x4");
  this->client->connect_for_test();
  this->client->drop_for_test(Reason::TCP_DISCONNECTED);
  EXPECT_EQ(c.last_error(), MqttError::CONNECTION_LOST);  // the logged refusal went with the connect
  this->client->connect_for_test();
  EXPECT_EQ(c.last_error(), MqttError::NONE);
}

TEST_F(StatusTest, TheLiveStatusMatchesTheLoopsView) {
  TestConfig &c = this->running();
  auto check = [&c]() {
    const MqttConfig::LiveStatus live = c.live_status();
    EXPECT_EQ(live.running, c.running());
    EXPECT_EQ(live.connected, c.connected());
    EXPECT_EQ(live.state, c.state());
    EXPECT_EQ(live.last_error, c.last_error());
  };
  check();
  this->client->connect_for_test();
  check();
  this->client->drop_for_test(Reason::DNS_RESOLVE_ERROR);
  check();
  EXPECT_EQ(c.live_status().state, MqttState::DISCONNECTED);
  EXPECT_EQ(c.live_status().last_error, MqttError::DNS);
}

// Other tasks read the status as one word, so every combination has to survive the trip.
TEST(MqttConfigStatus, EveryCombinationRoundTripsThroughThePackedWord) {
  std::set<uint32_t> seen;
  for (int flags = 0; flags < 8; flags++) {
    for (uint8_t state = 0; state <= static_cast<uint8_t>(MqttState::DISCONNECTED); state++) {
      for (uint8_t error = 0; error <= static_cast<uint8_t>(MqttError::CRASH_GUARD); error++) {
        const MqttConfig::LiveStatus status{(flags & 1) != 0, (flags & 2) != 0, static_cast<MqttState>(state),
                                            static_cast<MqttError>(error), (flags & 4) != 0};
        const uint32_t packed = MqttConfig::pack_status(status);
        EXPECT_EQ(MqttConfig::unpack_status(packed), status) << packed;
        seen.insert(packed);
      }
    }
  }
  EXPECT_EQ(seen.size(), 8u * 5u * 10u);
  // Nothing published yet reads as an idle, unconfigured client.
  EXPECT_EQ(MqttConfig::unpack_status(0),
            (MqttConfig::LiveStatus{false, false, MqttState::NOT_CONFIGURED, MqttError::NONE, false}));
}

// A reader on another task sees only what one write published: one write per transition, and
// never a combination the contract rules out.
TEST_F(StatusTest, EachTransitionPublishesOneConsistentSnapshot) {
  MqttRecord stored = enabled_record();
  stored.discovery = true;
  stored.enabled = false;
  this->plant(stored);
  TestConfig &c = this->boot();
  size_t writes = c.status_writes.size();
  EXPECT_EQ(writes, 1u);  // setup
  const auto step = [&](const char *what, const std::function<void()> &transition) {
    transition();
    EXPECT_EQ(c.status_writes.size(), writes + 1) << what;
    writes = c.status_writes.size();
    EXPECT_EQ(c.status_writes.back(), c.live_status()) << what;
  };
  step("first enable", [&] { this->save(patch_of([](MqttPatch &p) { p.enabled = true; })); });
  step("connect", [&] { this->client->connect_for_test(); });
  step("drop", [&] { this->client->drop_for_test(Reason::TCP_DISCONNECTED); });
  step("reconnect", [&] { this->client->connect_for_test(); });
  step("later change", [&] { this->save(patch_of([](MqttPatch &p) { p.username = std::string("jxd"); })); });
  step("refused", [&] { this->client->drop_for_test(Reason::MQTT_NOT_AUTHORIZED); });
  step("back", [&] { this->client->connect_for_test(); });
  step("factory reset", [&] { c.before_factory_reset([] {}); });

  for (const MqttConfig::LiveStatus &s : c.status_writes) {
    EXPECT_EQ(s.connected, s.state == MqttState::CONNECTED);
    if (s.state == MqttState::CONNECTING)
      EXPECT_EQ(s.last_error, MqttError::NONE);
    if (s.state == MqttState::DISCONNECTED)
      EXPECT_NE(s.last_error, MqttError::NONE);
    if (!s.running)
      EXPECT_TRUE(s.state == MqttState::NOT_CONFIGURED || s.state == MqttState::OFF);
    else
      EXPECT_TRUE(s.state != MqttState::NOT_CONFIGURED && s.state != MqttState::OFF);
  }
  EXPECT_TRUE(c.status_writes.back().reboot_required);
}

TEST_F(StatusTest, TheSettingsAnswerCarriesTheState) {
  this->running();
  this->client->drop_for_test(Reason::DNS_RESOLVE_ERROR);
  JsonDocument doc = this->settings_json();
  EXPECT_EQ(doc["state"].as<std::string>(), "disconnected");
  EXPECT_EQ(doc["last_error"].as<std::string>(), "dns");
}

// One instance for the whole run goes through the real registration: the logger keeps the
// pointer it is given, and the suite sizes its list for this and LogCapture.
TEST(MqttConfigLogListener, ReadsTheClientsErrorLinesThroughTheLogger) {
  name_the_node();
  host::setup_preferences();
  static auto *client = new mqtt::MQTTClientComponent();  // NOLINT(cppcoreguidelines-owning-memory)
  static auto *config = [] {
    auto *c = new MqttConfig(client);  // NOLINT(cppcoreguidelines-owning-memory)
    c->setup();
    return c;
  }();
  ESP_LOGE("mqtt", "Connection refused error: 0x%x", 4);
  client->drop_for_test(Reason::TCP_DISCONNECTED);
  // Not started, so the drop is unexpected here; what matters is what the log line set.
  EXPECT_EQ(config->last_error(), MqttError::BAD_CREDENTIALS);
}

}  // namespace esphome::mqtt_config::testing
