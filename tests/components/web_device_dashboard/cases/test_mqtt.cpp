#include <chrono>
#include <thread>
#include "common.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/components/mqtt/mqtt_component.h"
#include "esphome/components/mqtt_config/mqtt_config.h"

namespace esphome::web_device_dashboard::testing {

using mqtt::MQTTClientDisconnectReason;
using mqtt_config::CrashGuardRecord;
using mqtt_config::MqttConfig;
using mqtt_config::StoredMqttV1;

static const char *const MQTT = "/api/device/mqtt";
static const char *const STATUS = "/api/device/status";
static const char *const CAPABILITIES = "/api/device/capabilities";
static const char *const REBOOT = "/api/device/system/reboot";
static const char *const FACTORY_RESET = "/api/device/system/factory-reset";
static const char *const ROLLBACK = "/api/device/system/rollback";
// The compiled node name, which is also the default topic prefix on a build without a MAC suffix.
static const char *const NODE = "dashboard-test";
static const char *const TURN_ON = R"({"enabled":true,"broker":"10.0.2.2"})";

// NVS, RTC memory and the reset reason, which outlive one MqttConfig the way they outlive a
// boot. NVS is a byte blob here: the host preferences keep nothing over 255 bytes.
struct MqttBoard {
  std::vector<uint8_t> nvs;
  CrashGuardRecord rtc{};
  bool panic{false};
  bool fail_store{false};
};

class TestMqttConfig : public MqttConfig {
 public:
  TestMqttConfig(mqtt::MQTTClientComponent *client, MqttBoard *board) : MqttConfig(client), board_(board) {}
  // Nothing the scheduler holds may outlive this object.
  void forget_schedule() {
    this->cancel_timeout("mqtt-guard");
    this->cancel_interval("mqtt-cleanup");
  }

 protected:
  bool load_record_(StoredMqttV1 &out) override {
    if (this->board_->nvs.size() != sizeof(StoredMqttV1))
      return false;
    std::memcpy(&out, this->board_->nvs.data(), sizeof(StoredMqttV1));
    return true;
  }
  bool store_(const StoredMqttV1 &stored) override {
    if (this->board_->fail_store)
      return false;
    const auto *bytes = reinterpret_cast<const uint8_t *>(&stored);
    this->board_->nvs.assign(bytes, bytes + sizeof(StoredMqttV1));
    return true;
  }
  bool panic_reset_() const override { return this->board_->panic; }
  CrashGuardRecord &guard_record_() override { return this->board_->rtc; }
  // The suite's own LogCapture holds the listener slot; the cases drive the client instead.
  void add_log_listener_() override {}

  MqttBoard *board_;
};

// Lets the wall clock pass and runs what the scheduler has due: after_cleanup polls on an
// interval.
static void advance(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  App.scheduler.call(millis());
}

// The dashboard on a firmware with mqtt_config: the client as features/mqtt.yaml compiles it,
// the component, and an MQTT component per entity, set up in boot order (210, 200, 100).
class MqttDashboard : public Dashboard {
 protected:
  void SetUp() override {
    Dashboard::SetUp();
    this->boot();
  }
  void TearDown() override {
    // Whatever the dashboard deferred runs while the component it reaches is still there.
    Dashboard::TearDown();
    this->shutdown();
  }

  TestMqttConfig &boot(const std::function<void()> &before_setup = nullptr) {
    this->shutdown();
    this->client = std::make_unique<mqtt::MQTTClientComponent>();
    this->client->set_broker_address("");
    this->client->set_enable_on_boot(false);
    this->client->set_clean_session(true);
    this->client->set_discovery_info("homeassistant", mqtt::MQTT_MAC_ADDRESS_UNIQUE_ID_GENERATOR,
                                     mqtt::MQTT_NONE_OBJECT_ID_GENERATOR, true, false);
    this->client->set_topic_prefix(NODE, NODE);
    this->client->set_birth_message(mqtt::MQTTMessage{std::string(NODE) + "/status", "online", 0, true});
    this->client->set_last_will(mqtt::MQTTMessage{std::string(NODE) + "/status", "offline", 0, true});
    this->client->disable_log_message();
    this->client->set_reboot_timeout(0);

    this->config = std::make_unique<TestMqttConfig>(this->client.get(), &this->mqtt_board);
    TestMqttConfig &c = *this->config;
    c.set_preference_hash(fnv1_hash("mqtt_settings"));
    c.set_birth_template("online", 0, true);
    c.set_will_template("offline", 0, true);
    Entities &e = entities();
    this->relay = std::make_unique<mqtt::MQTTSwitchComponent>(&e.relay1);
    this->input = std::make_unique<mqtt::MQTTBinarySensorComponent>(&e.in1);
    c.reserve_entities(2);
    c.add_entity(this->relay.get(), &e.relay1);
    c.add_entity(this->input.get(), &e.in1);
    if (before_setup)
      before_setup();

    c.setup();
    this->client->setup();
    this->relay->call_setup();
    this->input->call_setup();
    return c;
  }
  // The next boot: NVS and RTC stay, everything else is built again.
  TestMqttConfig &reboot_device(const std::function<void()> &before_setup = nullptr) {
    return this->boot(before_setup);
  }

  // Cancelled, then left allocated: the scheduler drops a cancelled entry only once it comes
  // due, and asks the component it names whether it failed before it looks at the cancellation.
  void shutdown() {
    if (this->config != nullptr) {
      this->config->forget_schedule();
      (void) this->config.release();  // NOLINT(bugprone-unused-return-value)
    }
    // The firmware without the component, as far as the routes can tell.
    mqtt_config::global_mqtt_config = nullptr;
  }

  // The client connects and runs its resend passes until nothing is pending.
  void connect_and_settle() {
    this->client->connect_for_test();
    this->settle();
  }
  void settle() {
    while (this->client->process_resends_for_test() > 0) {
    }
  }
  // Discovery configs that went out empty: the removals.
  size_t removals() const {
    return std::count_if(this->client->published.begin(), this->client->published.end(),
                         [](const mqtt::MQTTClientComponent::Published &p) {
                           return p.topic.rfind("homeassistant/", 0) == 0 && p.payload.empty();
                         });
  }

  // Running, connected and announced to Home Assistant, as a boot with discovery on leaves it.
  void announced() {
    ASSERT_EQ(this->post(MQTT, R"({"enabled":true,"broker":"10.0.2.2","discovery":true})").code, 200);
    this->reboot_device();
    this->connect_and_settle();
    ASSERT_TRUE(this->client->is_discovery_enabled());
  }

  Reply post_mqtt(const std::string &body) { return this->post(MQTT, body); }

  MqttBoard mqtt_board;
  std::unique_ptr<mqtt::MQTTClientComponent> client;
  std::unique_ptr<TestMqttConfig> config;
  std::unique_ptr<mqtt::MQTTSwitchComponent> relay;
  std::unique_ptr<mqtt::MQTTBinarySensorComponent> input;
};

// --- GET ---

TEST_F(MqttDashboard, AFreshDeviceAnswersTheDefaults) {
  Reply reply = this->get(MQTT);
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.type, "application/json");
  EXPECT_FALSE(reply["enabled"].as<bool>());
  EXPECT_EQ(reply["broker"].as<std::string>(), "");
  EXPECT_EQ(reply["port"].as<int>(), 1883);
  EXPECT_EQ(reply["username"].as<std::string>(), "");
  EXPECT_FALSE(reply["password_set"].as<bool>());
  EXPECT_EQ(reply["client_id"].as<std::string>(), "");
  EXPECT_EQ(reply["client_id_default"].as<std::string>(), this->client->credentials().client_id);
  EXPECT_EQ(reply["topic_prefix"].as<std::string>(), "");
  EXPECT_EQ(reply["topic_prefix_default"].as<std::string>(), NODE);
  EXPECT_FALSE(reply["discovery"].as<bool>());
  EXPECT_EQ(reply["state"].as<std::string>(), "not_configured");
  EXPECT_TRUE(reply["last_error"].isNull());
  EXPECT_FALSE(reply["last_error"].isUnbound());
  EXPECT_TRUE(reply["running"].isNull());
  EXPECT_FALSE(reply["running"].isUnbound());
  EXPECT_TRUE(reply["apply_now"].as<bool>());
  EXPECT_FALSE(reply["reboot_required"].as<bool>());
  EXPECT_EQ(reply["discovery_cleanup"].as<std::string>(), "none");
  EXPECT_TRUE(reply["stored_notice"].isNull());
  EXPECT_FALSE(reply["stored_notice"].isUnbound());
}

TEST_F(MqttDashboard, TheReadSaysAPasswordIsSetAndNeverWhatItIs) {
  ASSERT_EQ(this->post_mqtt(R"({"username":"jxd","password":"s3cret-phrase"})").code, 200);
  Reply reply = this->get(MQTT);
  EXPECT_EQ(reply["username"].as<std::string>(), "jxd");
  EXPECT_TRUE(reply["password_set"].as<bool>());
  EXPECT_TRUE(reply["password"].isUnbound());
  EXPECT_EQ(reply.body.find("s3cret-phrase"), std::string::npos) << reply.body;
}

TEST_F(MqttDashboard, TheReadIsBuiltOnTheLoopTask) {
  ASSERT_EQ(this->get(MQTT).code, 200);
  EXPECT_EQ(this->dashboard->jobs, 1);
}

TEST_F(MqttDashboard, TheReadIsBusyWhenTheLoopDoesNotTakeIt) {
  this->dashboard->loop_busy = true;
  Reply reply = this->get(MQTT);
  EXPECT_EQ(reply.code, 503);
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "Device busy");
}

TEST_F(MqttDashboard, TheReadShowsWhatRunsOnceStarted) {
  ASSERT_EQ(this->post_mqtt(TURN_ON).code, 200);
  Reply reply = this->get(MQTT);
  EXPECT_TRUE(reply["enabled"].as<bool>());
  EXPECT_EQ(reply["broker"].as<std::string>(), "10.0.2.2");
  EXPECT_EQ(reply["state"].as<std::string>(), "connecting");
  EXPECT_FALSE(reply["apply_now"].as<bool>());
  JsonObject running = reply["running"].as<JsonObject>();
  ASSERT_FALSE(running.isNull());
  EXPECT_EQ(running["broker"].as<std::string>(), "10.0.2.2");
  EXPECT_EQ(running["port"].as<int>(), 1883);
  EXPECT_EQ(running["client_id"].as<std::string>(), this->client->credentials().client_id);
  EXPECT_EQ(running["topic_prefix"].as<std::string>(), NODE);
  EXPECT_EQ(running["status_topic"].as<std::string>(), std::string(NODE) + "/status");
  EXPECT_FALSE(running["discovery"].as<bool>());
}

// --- methods ---

TEST_F(MqttDashboard, TheRouteTakesBothMethodsAndRefusesTheRest) {
  for (http_method method : {HTTP_PUT, HTTP_DELETE, HTTP_HEAD}) {
    LogCapture::instance().clear();
    Reply reply = this->call(method, MQTT, TURN_ON);
    EXPECT_EQ(reply.code, 405) << method;
    EXPECT_EQ(reply.error(), "Method not allowed") << method;
    EXPECT_TRUE(LogCapture::instance().has_warning("allowed: GET, POST")) << method;
  }
  EXPECT_TRUE(this->mqtt_board.nvs.empty());
}

TEST_F(MqttDashboard, OnlyTheExactNameIsTheRoute) {
  for (const char *url : {"/api/device/mqtt/", "/api/device/mqtt/extra", "/api/device/mqt", "/api/device/mqtts"}) {
    Reply reply = this->post(url, TURN_ON);
    EXPECT_EQ(reply.code, 404) << url;
    EXPECT_EQ(reply.error(), "Not found") << url;
  }
  EXPECT_EQ(this->dashboard->route_for_(MQTT)->id, RouteId::MQTT);
}

// --- POST, refused in the order the contract gives ---

// The only one of the three form encodings the server hands on as a raw body, so the one a
// cross-site form would arrive as; no HTML form can send the type that passes.
TEST_F(MqttDashboard, AWriteRefusesABodyThatDoesNotSayItIsJson) {
  for (const char *type : {"text/plain", "", "application/x-www-form-urlencoded", "application/jsonp"}) {
    Reply reply = this->call(HTTP_POST, MQTT, TURN_ON, 512, type);
    EXPECT_EQ(reply.code, 415) << type;
    EXPECT_EQ(reply.error(), "Expected Content-Type: application/json") << type;
  }
  EXPECT_EQ(this->dashboard->jobs, 0);
  EXPECT_EQ(this->client->enable_calls, 0);
}

TEST_F(MqttDashboard, TheTypeIsCheckedBeforeTheSize) {
  Reply reply = this->call(HTTP_POST, MQTT, std::string(5000, 'x'), 512, "text/plain");
  EXPECT_EQ(reply.code, 415);
}

TEST_F(MqttDashboard, AWriteRefusesAnOversizedBody) {
  const std::string body = R"({"enabled":true,"broker":")" + std::string(5000, 'b') + R"("})";
  Reply reply = this->post_mqtt(body);
  EXPECT_EQ(reply.code, 413);
  EXPECT_EQ(reply.error(), "Request body over 4 KiB");
  EXPECT_EQ(this->dashboard->jobs, 0);
}

TEST_F(MqttDashboard, AWriteRefusesABodyThatIsNotAnObject) {
  for (const char *body : {"", "not json", "[]", "42", "null", R"("enabled")", "{"}) {
    Reply reply = this->post_mqtt(body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), "Invalid JSON") << body;
  }
  EXPECT_EQ(this->dashboard->jobs, 0);
}

// The types are read before the hand-over, so a busy loop does not hide them.
TEST_F(MqttDashboard, AWriteRefusesAKeyOrATypeItDoesNotTake) {
  this->dashboard->loop_busy = true;
  const std::vector<std::pair<const char *, const char *>> cases = {
      {R"({"host":"10.0.2.2"})", "'host' is not an MQTT setting"},
      {R"({"enabled":true,"password_set":true})", "'password_set' is not an MQTT setting"},
      {R"({"enabled":"yes"})", "'enabled' must be true or false"},
      {R"({"enabled":1})", "'enabled' must be true or false"},
      {R"({"discovery":null})", "'discovery' must be true or false"},
      {R"({"port":1.5})", "'port' must be a whole number"},
      {R"({"port":"1883"})", "'port' must be a whole number"},
      {R"({"port":1e30})", "'port' must be a whole number"},
      {R"({"broker":5})", "'broker' must be a string"},
      {R"({"username":false})", "'username' must be a string"},
      {R"({"password":null})", "'password' must be a string"},
      {R"({"client_id":[]})", "'client_id' must be a string"},
      {R"({"topic_prefix":{}})", "'topic_prefix' must be a string"},
  };
  for (const auto &[body, message] : cases) {
    Reply reply = this->post_mqtt(body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_FALSE(reply.success()) << body;
    EXPECT_EQ(reply.error(), message) << body;
  }
  EXPECT_EQ(this->dashboard->jobs, 0);
}

// The values are judged on the loop task, so a busy loop answers before them.
TEST_F(MqttDashboard, AWriteTheLoopDoesNotTakeChangesNothing) {
  this->dashboard->loop_busy = true;
  for (const char *body : {TURN_ON, R"({"port":70000})"}) {
    Reply reply = this->post_mqtt(body);
    EXPECT_EQ(reply.code, 503) << body;
    EXPECT_EQ(reply.error(), "Device busy") << body;
  }
  EXPECT_EQ(this->dashboard->jobs, 2);
  EXPECT_TRUE(this->mqtt_board.nvs.empty());
  EXPECT_EQ(this->client->enable_calls, 0);
  this->dashboard->loop_busy = false;
  EXPECT_FALSE(this->get(MQTT)["enabled"].as<bool>());
}

// This suite builds with IPv6, the devices without: only what both refuse is asserted here.
TEST_F(MqttDashboard, AWritePassesTheSettingsRefusalOn) {
  const std::vector<std::pair<const char *, const char *>> cases = {
      {R"({"port":70000})", "'port' must be between 1 and 65535"},
      {R"({"port":0})", "'port' must be between 1 and 65535"},
      {R"({"port":-1})", "'port' must be between 1 and 65535"},
      {R"({"enabled":true})", "'broker' is required to turn MQTT on"},
      {R"({"broker":"mqtt://10.0.2.2"})", "'broker' takes a host name, not a URL: drop the 'mqtt://'"},
      {R"({"broker":"host:1883"})", "'broker' cannot carry a port: put it in 'port'"},
      {R"({"broker":"my broker"})", "'broker' must be a host name or an IPv4 address"},
      {R"({"password":"secret"})", "'password' needs a 'username': MQTT 3.1.1 sends none without one"},
      {R"({"client_id":"has space"})", "'client_id' must be printable ASCII without spaces"},
      {R"({"topic_prefix":"home/#"})", "'topic_prefix' cannot contain '+' or '#'"},
      {R"({"topic_prefix":"home/"})", "'topic_prefix' cannot end with '/'"},
  };
  for (const auto &[body, message] : cases) {
    Reply reply = this->post_mqtt(body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), message) << body;
  }
  EXPECT_TRUE(this->mqtt_board.nvs.empty());
  EXPECT_EQ(this->client->enable_calls, 0);
}

TEST_F(MqttDashboard, AFailedStoreKeepsTheOldSettings) {
  this->mqtt_board.fail_store = true;
  Reply reply = this->post_mqtt(TURN_ON);
  EXPECT_EQ(reply.code, 500);
  EXPECT_EQ(reply.error(), "Storing the MQTT settings failed; the old ones stay in force");
  EXPECT_EQ(this->client->enable_calls, 0);
  this->mqtt_board.fail_store = false;
  Reply read = this->get(MQTT);
  EXPECT_FALSE(read["enabled"].as<bool>());
  EXPECT_EQ(read["broker"].as<std::string>(), "");
}

// --- POST, answered ---

TEST_F(MqttDashboard, TheFirstEnableConnectsAtOnce) {
  Reply reply = this->post_mqtt(TURN_ON);
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Saved; MQTT is connecting");
  EXPECT_TRUE(reply["started"].as<bool>());
  EXPECT_FALSE(reply["reboot_required"].as<bool>());
  EXPECT_EQ(reply["discovery_cleanup"].as<std::string>(), "none");
  EXPECT_EQ(this->client->enable_calls, 1);
  EXPECT_EQ(this->client->credentials().address, "10.0.2.2");
  EXPECT_EQ(this->dashboard->jobs, 1);
}

TEST_F(MqttDashboard, ALaterChangeWaitsForAReboot) {
  ASSERT_EQ(this->post_mqtt(TURN_ON).code, 200);
  Reply reply = this->post_mqtt(R"({"port":1884})");
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Saved; applies after a reboot");
  EXPECT_FALSE(reply["started"].as<bool>());
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
  EXPECT_EQ(this->client->enable_calls, 1);
  EXPECT_EQ(this->client->credentials().port, 1883);
  EXPECT_EQ(this->get(MQTT)["port"].as<int>(), 1884);

  this->reboot_device();
  EXPECT_EQ(this->client->credentials().port, 1884);
  EXPECT_FALSE(this->get(MQTT)["reboot_required"].as<bool>());
}

TEST_F(MqttDashboard, TheSameSettingsAgainChangeNothing) {
  ASSERT_EQ(this->post_mqtt(TURN_ON).code, 200);
  for (const char *body : {"{}", TURN_ON, R"({"port":1883,"discovery":false})"}) {
    Reply reply = this->post_mqtt(body);
    ASSERT_EQ(reply.code, 200) << body;
    EXPECT_EQ(reply.message(), "Nothing changed") << body;
    EXPECT_FALSE(reply["started"].as<bool>()) << body;
    EXPECT_FALSE(reply["reboot_required"].as<bool>()) << body;
  }
  EXPECT_EQ(this->client->enable_calls, 1);
}

TEST_F(MqttDashboard, DiscoveryOffRemovesTheEntriesWithoutAReboot) {
  this->announced();
  Reply reply = this->post_mqtt(R"({"discovery":false})");
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Saved; removing this device's Home Assistant entries");
  EXPECT_EQ(reply["discovery_cleanup"].as<std::string>(), "running");
  EXPECT_FALSE(reply["reboot_required"].as<bool>());
  this->settle();
  EXPECT_EQ(this->removals(), 2u);
}

TEST_F(MqttDashboard, DiscoveryOffWhileDisconnectedWaitsForTheBroker) {
  this->announced();
  this->client->drop_for_test(MQTTClientDisconnectReason::TCP_DISCONNECTED);
  Reply reply = this->post_mqtt(R"({"discovery":false})");
  EXPECT_EQ(reply.message(), "Saved; this device's Home Assistant entries go once the broker is reachable");
  EXPECT_EQ(reply["discovery_cleanup"].as<std::string>(), "pending");
}

TEST_F(MqttDashboard, MqttOffWhileDisconnectedSaysTheEntriesStay) {
  this->announced();
  this->client->drop_for_test(MQTTClientDisconnectReason::TCP_DISCONNECTED);
  Reply reply = this->post_mqtt(R"({"enabled":false})");
  EXPECT_EQ(reply.message(),
            "Saved; applies after a reboot. The broker is not reachable, so this device's Home Assistant entries "
            "stay on it unless it comes back before then");
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
  EXPECT_EQ(reply["discovery_cleanup"].as<std::string>(), "pending");
}

// --- /status ---

TEST_F(MqttDashboard, StatusCarriesTheClientAndNothingWaits) {
  Reply reply = this->get(STATUS);
  ASSERT_EQ(reply.code, 200);
  JsonObject mqtt = reply["mqtt"].as<JsonObject>();
  ASSERT_FALSE(mqtt.isNull());
  EXPECT_TRUE(mqtt["available"].as<bool>());
  EXPECT_FALSE(mqtt["enabled"].as<bool>());
  EXPECT_FALSE(mqtt["connected"].as<bool>());
  EXPECT_EQ(mqtt["state"].as<std::string>(), "not_configured");
  EXPECT_TRUE(mqtt["last_error"].isNull());
  EXPECT_FALSE(mqtt["last_error"].isUnbound());
  EXPECT_FALSE(reply["reboot_required"].as<bool>());
  EXPECT_TRUE(reply["reboot_reasons"].isUnbound());
  // Atomics only: the status is read where the request is, not on the loop task.
  EXPECT_EQ(this->dashboard->jobs, 0);
}

TEST_F(MqttDashboard, StatusFollowsTheConnection) {
  ASSERT_EQ(this->post_mqtt(TURN_ON).code, 200);
  Reply connecting = this->get(STATUS);
  EXPECT_TRUE(connecting["mqtt"]["enabled"].as<bool>());
  EXPECT_EQ(connecting["mqtt"]["state"].as<std::string>(), "connecting");

  this->connect_and_settle();
  Reply up = this->get(STATUS);
  EXPECT_TRUE(up["mqtt"]["connected"].as<bool>());
  EXPECT_EQ(up["mqtt"]["state"].as<std::string>(), "connected");
  EXPECT_TRUE(up["mqtt"]["last_error"].isNull());
  EXPECT_FALSE(up["reboot_required"].as<bool>());

  this->client->drop_for_test(MQTTClientDisconnectReason::TCP_DISCONNECTED);
  Reply lost = this->get(STATUS);
  EXPECT_FALSE(lost["mqtt"]["connected"].as<bool>());
  EXPECT_EQ(lost["mqtt"]["state"].as<std::string>(), "disconnected");
  EXPECT_EQ(lost["mqtt"]["last_error"].as<std::string>(), "connection_lost");

  this->client->drop_for_test(MQTTClientDisconnectReason::MQTT_NOT_AUTHORIZED);
  EXPECT_EQ(this->get(STATUS)["mqtt"]["last_error"].as<std::string>(), "not_authorized");
  this->client->drop_for_test(MQTTClientDisconnectReason::DNS_RESOLVE_ERROR);
  EXPECT_EQ(this->get(MQTT)["last_error"].as<std::string>(), "dns");
}

TEST_F(MqttDashboard, StatusNamesMqttWhileAChangeWaits) {
  ASSERT_EQ(this->post_mqtt(TURN_ON).code, 200);
  ASSERT_EQ(this->post_mqtt(R"({"port":1884})").code, 200);
  Reply reply = this->get(STATUS);
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
  JsonArray reasons = reply["reboot_reasons"].as<JsonArray>();
  ASSERT_EQ(reasons.size(), 1u);
  EXPECT_EQ(reasons[0].as<std::string>(), "mqtt");

  // Back to what runs: nothing waits any more.
  ASSERT_EQ(this->post_mqtt(R"({"port":1883})").code, 200);
  Reply undone = this->get(STATUS);
  EXPECT_FALSE(undone["reboot_required"].as<bool>());
  EXPECT_TRUE(undone["reboot_reasons"].isUnbound());
}

// A prefix change cannot start live: the command topics were subscribed with the boot's.
TEST_F(MqttDashboard, AFirstEnableWithANewPrefixWaitsForTheReboot) {
  Reply reply = this->post_mqtt(R"({"enabled":true,"broker":"10.0.2.2","topic_prefix":"house"})");
  EXPECT_EQ(reply.message(), "Saved; applies after a reboot");
  EXPECT_FALSE(reply["started"].as<bool>());
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
  EXPECT_EQ(this->client->enable_calls, 0);
  EXPECT_EQ(this->get(STATUS)["reboot_reasons"][0].as<std::string>(), "mqtt");
  EXPECT_EQ(this->get(STATUS)["mqtt"]["state"].as<std::string>(), "off");
}

// Held back after repeated crashes: Reboot now is what retries it, so a restart waits.
TEST_F(MqttDashboard, StatusNamesMqttWhileItIsHeldBack) {
  ASSERT_EQ(this->post_mqtt(TURN_ON).code, 200);
  this->reboot_device([this]() {
    this->mqtt_board.rtc = CrashGuardRecord{mqtt_config::CRASH_GUARD_MAGIC, 2, 1, {0, 0}};
    this->mqtt_board.panic = true;
  });
  Reply status = this->get(STATUS);
  EXPECT_FALSE(status["mqtt"]["enabled"].as<bool>());
  EXPECT_EQ(status["mqtt"]["state"].as<std::string>(), "off");
  EXPECT_EQ(status["mqtt"]["last_error"].as<std::string>(), "crash_guard");
  EXPECT_TRUE(status["reboot_required"].as<bool>());
  EXPECT_EQ(status["reboot_reasons"][0].as<std::string>(), "mqtt");
  EXPECT_FALSE(this->get(MQTT)["apply_now"].as<bool>());
  EXPECT_EQ(this->client->enable_calls, 0);
}

// --- /capabilities ---

TEST_F(MqttDashboard, CapabilitiesSayTheRouteIsServed) {
  Reply reply = this->get(CAPABILITIES);
  ASSERT_TRUE(reply["mqtt"].is<bool>());
  EXPECT_TRUE(reply["mqtt"].as<bool>());
}

// --- the system actions wait for the Home Assistant cleanup ---

TEST_F(MqttDashboard, ARebootWithNothingToRemoveRestartsAtOnce) {
  ASSERT_EQ(this->post_mqtt(TURN_ON).code, 200);
  this->connect_and_settle();
  ASSERT_EQ(this->post(REBOOT, this->confirmation()).code, 200);
  Dashboard::loop();
  EXPECT_EQ(this->dashboard->restarts, 1);
}

TEST_F(MqttDashboard, ARebootWaitsForTheEntriesToLeave) {
  this->announced();
  ASSERT_EQ(this->post_mqtt(R"({"discovery":false})")["discovery_cleanup"].as<std::string>(), "running");
  Reply reply = this->post(REBOOT, this->confirmation());
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Rebooting");
  Dashboard::loop();
  advance(150);
  EXPECT_EQ(this->dashboard->restarts, 0);

  this->settle();
  EXPECT_EQ(this->removals(), 2u);
  advance(150);
  EXPECT_EQ(this->dashboard->restarts, 1);
}

// A dropped connection ends the wait: nothing more goes out until it is back.
TEST_F(MqttDashboard, ARebootStopsWaitingWhenTheBrokerGoes) {
  this->announced();
  ASSERT_EQ(this->post_mqtt(R"({"discovery":false})").code, 200);
  ASSERT_EQ(this->post(REBOOT, this->confirmation()).code, 200);
  Dashboard::loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  this->client->drop_for_test(MQTTClientDisconnectReason::TCP_DISCONNECTED);
  advance(150);
  EXPECT_EQ(this->dashboard->restarts, 1);
}

TEST_F(MqttDashboard, ARollbackWaitsToo) {
  this->dashboard->stub_rollback = true;
  this->dashboard->rollback.partition = "app1";
  this->announced();
  ASSERT_EQ(this->post_mqtt(R"({"discovery":false})").code, 200);
  ASSERT_EQ(this->post(ROLLBACK, this->confirmation()).code, 200);
  Dashboard::loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  this->settle();
  advance(150);
  EXPECT_EQ(this->dashboard->restarts, 1);
}

// The record goes with NVS, so the entries leave the broker before the wipe.
TEST_F(MqttDashboard, AFactoryResetRemovesTheEntriesFirst) {
  this->announced();
  ASSERT_EQ(this->post(FACTORY_RESET, this->confirmation()).code, 200);
  Dashboard::loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(this->storage.format_requests, 0);
  EXPECT_FALSE(this->mqtt_board.nvs.empty());
  EXPECT_EQ(this->get(MQTT)["discovery_cleanup"].as<std::string>(), "running");

  this->settle();
  EXPECT_EQ(this->removals(), 2u);
  advance(150);
  EXPECT_EQ(this->dashboard->restarts, 1);
  EXPECT_EQ(this->storage.format_requests, 1);
}

TEST_F(MqttDashboard, AFactoryResetWithoutDiscoveryWipesAtOnce) {
  ASSERT_EQ(this->post_mqtt(TURN_ON).code, 200);
  this->connect_and_settle();
  ASSERT_EQ(this->post(FACTORY_RESET, this->confirmation()).code, 200);
  Dashboard::loop();
  EXPECT_EQ(this->dashboard->restarts, 1);
  EXPECT_EQ(this->storage.format_requests, 1);
  EXPECT_EQ(this->removals(), 0u);
}

// --- a firmware whose component did not come up ---

// Codegen constructs the component before any route is served, so a device never gets here;
// the routes then answer as the other components' routes do, and the rest leaves MQTT out.
TEST_F(Dashboard, MqttRoutesSayWhenTheComponentIsMissing) {
  ASSERT_EQ(mqtt_config::global_mqtt_config, nullptr);
  Reply read = this->get(MQTT);
  EXPECT_EQ(read.code, 503);
  EXPECT_EQ(read.error(), "MQTT not available");
  Reply write = this->post(MQTT, TURN_ON);
  EXPECT_EQ(write.code, 503);
  EXPECT_EQ(write.error(), "MQTT not available");
  EXPECT_EQ(this->dashboard->jobs, 0);

  Reply status = this->get(STATUS);
  EXPECT_TRUE(status["mqtt"].isUnbound());
  EXPECT_FALSE(status["reboot_required"].as<bool>());
  EXPECT_TRUE(status["reboot_reasons"].isUnbound());
  EXPECT_TRUE(this->get(CAPABILITIES)["mqtt"].isUnbound());

  ASSERT_EQ(this->post(REBOOT, this->confirmation()).code, 200);
  Dashboard::loop();
  EXPECT_EQ(this->dashboard->restarts, 1);
  ASSERT_EQ(this->post(FACTORY_RESET, this->confirmation()).code, 200);
  Dashboard::loop();
  EXPECT_EQ(this->dashboard->restarts, 2);
}

}  // namespace esphome::web_device_dashboard::testing
