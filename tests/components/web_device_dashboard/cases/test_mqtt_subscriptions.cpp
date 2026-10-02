#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include "common.h"
#include "entity_tables.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/components/mqtt_config/mqtt_config.h"
#include "esphome/components/mqtt_subscriptions/mqtt_subscriptions.h"

namespace esphome::web_device_dashboard::testing {

using mqtt_subscriptions::MqttSubscriptions;

static const char *const SLOTS = "/api/device/mqtt/subscriptions";
static const char *const SLOTS_STATUS = "/api/device/status";
static const char *const SLOTS_CAPABILITIES = "/api/device/capabilities";
static const char *const OUTDOOR =
    R"({"slot":1,"enabled":true,"name":"Outdoor","topic":"zigbee2mqtt/outdoor","kind":"sensor",)"
    R"("json_path":"temperature","unit":"°C"})";

// The RTC record and the reset reason the crash guard reads; nothing is ever stored in NVS here.
struct SlotsBoard {
  mqtt_config::CrashGuardRecord rtc{};
  bool panic{false};
};

class SlotsMqttConfig : public mqtt_config::MqttConfig {
 public:
  SlotsMqttConfig(mqtt::MQTTClientComponent *client, SlotsBoard *board) : MqttConfig(client), board_(board) {}
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

  SlotsBoard *board_;
};

class TestSlots : public MqttSubscriptions {
 public:
  void forget_schedule() { this->cancel_interval("mqtt-subs-check"); }
};

// The dashboard on a firmware with mqtt_subscriptions: the client, mqtt_config and two slots
// over a directory of their own, set up in boot order (803, 210, 200).
class SlotsDashboard : public Dashboard {
 protected:
  void SetUp() override {
    Dashboard::SetUp();
    this->tables = esphome::testing::EntityTableMark();
    mkdir(".storage", 0755);
    char folder[] = ".storage/slotsXXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->slot_storage.set_base_path(folder);
    this->slot_storage.setup();
    this->boot();
  }
  void TearDown() override {
    Dashboard::TearDown();
    this->shutdown();
    this->tables.restore();
    std::error_code ignored;
    std::filesystem::remove_all(this->slot_storage.get_base_path(), ignored);
  }

  TestSlots &boot(filesystem_storage_abstract::FilesystemStorageAbstract *storage = nullptr) {
    this->shutdown();
    this->tables.restore();
    this->client = std::make_unique<mqtt::MQTTClientComponent>();
    this->client->set_enable_on_boot(false);
    this->config = std::make_unique<SlotsMqttConfig>(this->client.get(), &this->slots_board);
    this->config->set_preference_hash(fnv1_hash("mqtt_settings"));
    this->subs = std::make_unique<TestSlots>();
    this->subs->set_config(this->config.get());
    this->subs->set_storage(storage != nullptr ? storage : &this->slot_storage);
    this->subs->set_folder_path("mqtt");
    this->subs->set_max_slots(2);
    this->subs->add_unit("°C", 1);
    this->subs->setup();
    this->config->setup();
    this->client->setup();
    return *this->subs;
  }

  // Cancelled and left allocated: the scheduler asks a cancelled entry's component whether it
  // failed before it drops the entry. The routes then see a firmware without the components.
  void shutdown() {
    if (this->subs != nullptr) {
      this->subs->forget_schedule();
      (void) this->subs.release();  // NOLINT(bugprone-unused-return-value)
    }
    if (this->config != nullptr) {
      this->config->forget_schedule();
      (void) this->config.release();  // NOLINT(bugprone-unused-return-value)
    }
    mqtt_subscriptions::global_mqtt_subscriptions = nullptr;
    mqtt_config::global_mqtt_config = nullptr;
    this->client.reset();
  }

  std::string file() const { return this->slot_storage.get_base_path() + "/mqtt/subscriptions.json"; }
  void plant(const std::string &text) {
    mkdir((this->slot_storage.get_base_path() + "/mqtt").c_str(), 0755);
    std::ofstream(this->file(), std::ios::trunc) << text;
  }
  std::string file_text() const {
    std::ifstream in(this->file());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  Reply post_slot(const std::string &body) { return this->post(SLOTS, body); }

  SlotsBoard slots_board;
  // Every boot here creates its slots' entities anew.
  esphome::testing::EntityTableMark tables;
  dir_storage::DirStorage slot_storage;
  std::unique_ptr<mqtt::MQTTClientComponent> client;
  std::unique_ptr<SlotsMqttConfig> config;
  std::unique_ptr<TestSlots> subs;
};

// --- GET ---

TEST_F(SlotsDashboard, AFreshDeviceListsEveryEmptySlot) {
  Reply reply = this->get(SLOTS);
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.type, "application/json");
  EXPECT_EQ(reply["max_slots"].as<int>(), 2);
  EXPECT_FALSE(reply["reboot_required"].as<bool>());
  EXPECT_FALSE(reply["suspended"].as<bool>());
  EXPECT_TRUE(reply["file_error"].isNull());
  EXPECT_FALSE(reply["file_error"].isUnbound());
  ASSERT_EQ(reply["units"].size(), 1u);
  EXPECT_EQ(reply["units"][0].as<std::string>(), "°C");
  // No probes on this firmware, so no names are kept for them.
  EXPECT_TRUE(reply["reserved_names"].isNull());
  EXPECT_FALSE(reply["reserved_names"].isUnbound());
  ASSERT_EQ(reply["slots"].size(), 2u);
  JsonObject slot = reply["slots"][1];
  EXPECT_EQ(slot["slot"].as<int>(), 2);
  EXPECT_FALSE(slot["enabled"].as<bool>());
  EXPECT_EQ(slot["topic"].as<std::string>(), "");
  EXPECT_EQ(slot["kind"].as<std::string>(), "sensor");
  EXPECT_FALSE(slot["pending"].as<bool>());
  EXPECT_TRUE(slot["entity"].isNull());
  EXPECT_FALSE(slot["entity"].isUnbound());
  EXPECT_EQ(slot["status"]["state"].as<std::string>(), "off");
  EXPECT_TRUE(slot["status"]["age_s"].isNull());
  EXPECT_EQ(this->dashboard->jobs, 1);
}

TEST_F(SlotsDashboard, TheReadShowsWhatRunsAndWhatItRead) {
  this->plant(R"({"version":1,"slots":[{"slot":1,"enabled":true,"name":"Outdoor","topic":"t","kind":"sensor",)"
              R"("unit":"°C"}]})");
  this->boot();
  this->client->connect_for_test();
  this->client->deliver_for_test("t", "21.46");
  Reply reply = this->get(SLOTS);
  JsonObject slot = reply["slots"][0];
  EXPECT_EQ(slot["entity"]["domain"].as<std::string>(), "sensor");
  EXPECT_EQ(slot["entity"]["name"].as<std::string>(), "Outdoor");
  EXPECT_EQ(slot["status"]["state"].as<std::string>(), "ok");
  EXPECT_EQ(slot["status"]["value"].as<std::string>(), "21.5 °C");
  EXPECT_EQ(slot["status"]["raw"].as<std::string>(), "21.46");
  EXPECT_EQ(slot["status"]["age_s"].as<int>(), 0);
}

TEST_F(SlotsDashboard, TheReadIsBusyWhenTheLoopDoesNotTakeIt) {
  this->dashboard->loop_busy = true;
  Reply reply = this->get(SLOTS);
  EXPECT_EQ(reply.code, 503);
  EXPECT_EQ(reply.error(), "Device busy");
}

// --- methods and names ---

TEST_F(SlotsDashboard, TheRouteTakesBothMethodsAndRefusesTheRest) {
  for (http_method method : {HTTP_PUT, HTTP_DELETE, HTTP_HEAD}) {
    LogCapture::instance().clear();
    Reply reply = this->call(method, SLOTS, OUTDOOR);
    EXPECT_EQ(reply.code, 405) << method;
    EXPECT_EQ(reply.error(), "Method not allowed") << method;
    EXPECT_TRUE(LogCapture::instance().has_warning("allowed: GET, POST")) << method;
  }
  EXPECT_EQ(this->file_text(), "");
}

TEST_F(SlotsDashboard, OnlyTheExactNameIsTheRoute) {
  for (const char *url : {"/api/device/mqtt/subscriptions/", "/api/device/mqtt/subscription",
                          "/api/device/mqtt/subscriptions/1", "/api/device/mqtt/slots"}) {
    Reply reply = this->post(url, OUTDOOR);
    EXPECT_EQ(reply.code, 404) << url;
    EXPECT_EQ(reply.error(), "Not found") << url;
  }
  EXPECT_EQ(this->dashboard->route_for_(SLOTS)->id, RouteId::MQTT_SUBSCRIPTIONS);
}

// --- POST, refused in the order the contract gives ---

TEST_F(SlotsDashboard, AWriteRefusesABodyThatDoesNotSayItIsJson) {
  for (const char *type : {"text/plain", "", "application/x-www-form-urlencoded"}) {
    Reply reply = this->call(HTTP_POST, SLOTS, OUTDOOR, 512, type);
    EXPECT_EQ(reply.code, 415) << type;
    EXPECT_EQ(reply.error(), "Expected Content-Type: application/json") << type;
  }
  // Before the size.
  EXPECT_EQ(this->call(HTTP_POST, SLOTS, std::string(5000, 'x'), 512, "text/plain").code, 415);
  EXPECT_EQ(this->dashboard->jobs, 0);
}

TEST_F(SlotsDashboard, AWriteRefusesAnOversizedBody) {
  const std::string body = R"({"slot":1,"enabled":true,"name":")" + std::string(5000, 'n') + R"("})";
  Reply reply = this->post_slot(body);
  EXPECT_EQ(reply.code, 413);
  EXPECT_EQ(reply.error(), "Request body over 4 KiB");
  EXPECT_EQ(this->dashboard->jobs, 0);
}

TEST_F(SlotsDashboard, AWriteRefusesABodyThatIsNotAnObject) {
  for (const char *body : {"", "not json", "[]", "42", "null", "{"}) {
    Reply reply = this->post_slot(body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), "Invalid JSON") << body;
  }
  EXPECT_EQ(this->dashboard->jobs, 0);
}

TEST_F(SlotsDashboard, AWriteTheLoopDoesNotTakeChangesNothing) {
  this->dashboard->loop_busy = true;
  Reply reply = this->post_slot(OUTDOOR);
  EXPECT_EQ(reply.code, 503);
  EXPECT_EQ(reply.error(), "Device busy");
  EXPECT_EQ(this->file_text(), "");
}

TEST_F(SlotsDashboard, TheComponentsRefusalsComeThroughVerbatim) {
  const std::vector<std::pair<const char *, const char *>> cases = {
      {R"({"action":"clear"})", "'slot' must be a whole number from 1 to 2"},
      {R"({"slot":0,"action":"clear"})", "'slot' must be a whole number from 1 to 2"},
      {R"({"slot":3,"action":"clear"})", "'slot' must be a whole number from 1 to 2"},
      {R"({"slot":"1","action":"clear"})", "'slot' must be a whole number from 1 to 2"},
      {R"({"slot":1,"action":"wipe"})", "'action' must be 'clear'"},
      {R"({"slot":1,"enabled":"yes","name":"A","topic":"t","kind":"sensor"})", "'enabled' must be true or false"},
      {R"({"slot":1,"enabled":true,"name":"A","topic":"t","kind":"number"})",
       "'kind' must be 'sensor', 'binary_sensor' or 'text_sensor'"},
      {R"({"slot":1,"enabled":true,"name":"A","topic":"t","kind":"sensor","qos":1})", "'qos' is not a slot field"},
      {R"({"slot":1,"enabled":true,"name":"In/Out","topic":"t","kind":"sensor"})", "'name' cannot contain '/'"},
      {R"({"slot":1,"enabled":true,"name":"A","topic":"a/#","kind":"sensor"})",
       "'topic' cannot contain '+' or '#': a slot takes one topic"},
      {R"({"slot":1,"enabled":true,"name":"A","topic":"t","kind":"sensor","unit":"K"})",
       "'unit' is not one this firmware offers"},
      {R"({"slot":1,"enabled":true,"name":"A","topic":"t","kind":"sensor","decimals":260})",
       "'decimals' must be a whole number from 0 to 4"},
      {R"({"slot":1,"enabled":true,"name":"Relay 1","topic":"t","kind":"text_sensor"})", nullptr},
      {R"({"slot":2,"enabled":true,"name":"In 1","topic":"t","kind":"binary_sensor"})",
       "'name' gives the same id as the entity 'In 1'"},
  };
  for (const auto &[body, message] : cases) {
    Reply reply = this->post_slot(body);
    if (message == nullptr) {
      EXPECT_EQ(reply.code, 200) << body;  // a switch's name is free for a text sensor
      continue;
    }
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_FALSE(reply.success()) << body;
    EXPECT_EQ(reply.error(), message) << body;
  }
}

TEST_F(SlotsDashboard, ASaveAnswersWhatItDid) {
  Reply reply = this->post_slot(OUTDOOR);
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Saved; applies after a reboot");
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
  EXPECT_NE(this->file_text().find("zigbee2mqtt/outdoor"), std::string::npos);
  EXPECT_TRUE(this->get(SLOTS)["slots"][0]["pending"].as<bool>());

  reply = this->post_slot(OUTDOOR);
  EXPECT_EQ(reply.message(), "Nothing changed");
  reply = this->post_slot(R"({"slot":1,"action":"clear"})");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Cleared");
  EXPECT_FALSE(reply["reboot_required"].as<bool>());
}

TEST_F(SlotsDashboard, AClearOfARunningSlotSaysItsEntityGoesLater) {
  ASSERT_EQ(this->post_slot(OUTDOOR).code, 200);
  this->boot();
  Reply reply = this->post_slot(R"({"slot":1,"action":"clear"})");
  EXPECT_EQ(reply.message(), "Cleared; its entity goes after a reboot");
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
}

TEST_F(SlotsDashboard, NewerFirmwaresFileIsRefused) {
  const std::string newer = R"({"version":2,"slots":[]})";
  this->plant(newer);
  this->boot();
  Reply reply = this->post_slot(OUTDOOR);
  EXPECT_EQ(reply.code, 503);
  EXPECT_EQ(reply.error(), "The subscriptions file was written by newer firmware; it is left as it is");
  EXPECT_EQ(this->file_text(), newer);
  EXPECT_EQ(this->get(SLOTS)["file_error"].as<std::string>(), "newer_firmware");
}

TEST_F(SlotsDashboard, AStorageThatIsNotMountedIsUnavailable) {
  static dir_storage::DirStorage unmounted;  // never set up
  this->boot(&unmounted);
  Reply reply = this->post_slot(OUTDOOR);
  EXPECT_EQ(reply.code, 503);
  EXPECT_EQ(reply.error(), "Storage unavailable");
  EXPECT_EQ(this->get(SLOTS)["file_error"].as<std::string>(), "unavailable");
}

// --- /status and /capabilities ---

TEST_F(SlotsDashboard, CapabilitiesCarryTheSlotCount) {
  Reply reply = this->get(SLOTS_CAPABILITIES);
  EXPECT_EQ(reply["mqtt_subscriptions"]["max_slots"].as<int>(), 2);
  EXPECT_TRUE(reply["mqtt"].as<bool>());
}

// The slots alone are reason enough, under the one reason the MQTT notice reads.
TEST_F(SlotsDashboard, StatusNamesMqttWhileASlotWaits) {
  Reply status = this->get(SLOTS_STATUS);
  EXPECT_FALSE(status["reboot_required"].as<bool>());
  EXPECT_TRUE(status["reboot_reasons"].isUnbound());
  ASSERT_EQ(this->post_slot(OUTDOOR).code, 200);
  ASSERT_FALSE(this->config->reboot_required());
  status = this->get(SLOTS_STATUS);
  EXPECT_TRUE(status["reboot_required"].as<bool>());
  ASSERT_EQ(status["reboot_reasons"].size(), 1u);
  EXPECT_EQ(status["reboot_reasons"][0].as<std::string>(), "mqtt");
}

// The second crash in a row: a restart is what retries the subscriptions.
TEST_F(SlotsDashboard, StatusNamesMqttWhileTheSlotsAreSuspended) {
  this->slots_board.rtc = mqtt_config::CrashGuardRecord{mqtt_config::CRASH_GUARD_MAGIC, 1, 1, {0, 0}};
  this->slots_board.panic = true;
  this->boot();
  ASSERT_FALSE(this->config->reboot_required());
  Reply status = this->get(SLOTS_STATUS);
  EXPECT_TRUE(status["reboot_required"].as<bool>());
  EXPECT_EQ(status["reboot_reasons"][0].as<std::string>(), "mqtt");
  EXPECT_TRUE(this->get(SLOTS)["suspended"].as<bool>());
}

// --- a firmware whose component did not come up ---

TEST_F(Dashboard, SlotRoutesSayWhenTheComponentIsMissing) {
  ASSERT_EQ(mqtt_subscriptions::global_mqtt_subscriptions, nullptr);
  Reply read = this->get(SLOTS);
  EXPECT_EQ(read.code, 503);
  EXPECT_EQ(read.error(), "MQTT subscriptions not available");
  Reply write = this->post(SLOTS, OUTDOOR);
  EXPECT_EQ(write.code, 503);
  EXPECT_EQ(write.error(), "MQTT subscriptions not available");
  EXPECT_EQ(this->dashboard->jobs, 0);
  EXPECT_TRUE(this->get(SLOTS_CAPABILITIES)["mqtt_subscriptions"].isUnbound());
  EXPECT_FALSE(this->get(SLOTS_STATUS)["reboot_required"].as<bool>());
}

}  // namespace esphome::web_device_dashboard::testing
