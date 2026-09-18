#include "common.h"
#include "esphome/core/application.h"

namespace esphome::network_config::testing {

// The component over a record and a mode select. The interfaces are compiled out on the host,
// so what is left is the ordering, the select and the pending-reboot bookkeeping.
class Runtime : public ::testing::Test {
 protected:
  struct Wiring {
    NetworkSettingsJson settings;
    TestSelect mode_select;
    NetworkConfig config{&settings};
  };

  void SetUp() override {
    log().clear();
    reset_preferences();
    config_json::global_config_json_keeper = nullptr;
    // A deferred save leaves the scheduler holding a pointer to the component, so the wiring
    // outlives the test the way the other suites keep their keepers alive.
    static std::vector<std::unique_ptr<Wiring>> all;
    all.push_back(std::make_unique<Wiring>());
    this->wiring = all.back().get();
    this->settings().set_available_interfaces(true, true);
    this->config().set_mode_select(&this->mode_select());
  }

  void TearDown() override { App.scheduler.cancel_interval(&this->config(), "inbox"); }

  NetworkSettingsJson &settings() { return this->wiring->settings; }
  NetworkConfig &config() { return this->wiring->config; }
  TestSelect &mode_select() { return this->wiring->mode_select; }

  bool load(const std::string &text) {
    JsonDocument doc;
    if (deserializeJson(doc, text) != DeserializationError::Ok)
      return false;
    JsonObject root = doc.as<JsonObject>();
    return this->settings().parse_json(root, 1U);
  }

  const char *update(const std::string &text) {
    JsonDocument doc;
    EXPECT_EQ(deserializeJson(doc, text), DeserializationError::Ok) << text;
    return this->config().update_from_json(doc.as<JsonObject>());
  }

  Wiring *wiring{nullptr};
};

TEST_F(Runtime, SetsUpBetweenTheKeeperAndTheInterfaces) {
  const float priority = this->config().get_setup_priority();
  // After the config_json keeper has read the file, before wifi and ethernet set up.
  EXPECT_LT(priority, setup_priority::HARDWARE + 5.0f);
  EXPECT_GT(priority, setup_priority::WIFI);
  EXPECT_GT(priority, setup_priority::ETHERNET);
  EXPECT_FLOAT_EQ(priority, 260.0f);
}

TEST_F(Runtime, PublishesTheStoredModeIntoTheSelect) {
  ASSERT_TRUE(this->load(R"({"network_type":"wifi"})"));
  this->config().setup();
  ASSERT_TRUE(this->mode_select().active_index().has_value());
  EXPECT_EQ(this->mode_select().active_index().value(), 2U);
}

TEST_F(Runtime, TheDefaultModeReachesTheSelectToo) {
  this->config().setup();
  ASSERT_TRUE(this->mode_select().active_index().has_value());
  EXPECT_EQ(this->mode_select().active_index().value(), 1U);  // Ethernet
}

TEST_F(Runtime, TheSelectWritesTheModeBack) {
  this->config().setup();
  this->config().set_network_type_from_select(0);
  EXPECT_EQ(this->settings().network_type(), NetworkType::AUTO);
  EXPECT_TRUE(this->settings().is_dirty());
}

TEST_F(Runtime, AnIndexNoModeAnswersToIsIgnored) {
  this->config().setup();
  this->config().set_network_type_from_select(7);
  EXPECT_EQ(this->settings().network_type(), NetworkType::ETHERNET);
  EXPECT_TRUE(log().has_warning("no network mode"));
}

TEST_F(Runtime, NothingIsPendingRightAfterSetup) {
  ASSERT_TRUE(this->load(R"({"network_type":"auto","wifi_networks":[{"ssid":"home","password":"s3cret00"}]})"));
  this->config().setup();
  EXPECT_FALSE(this->config().reboot_required());
}

TEST_F(Runtime, AStoredChangeIsPendingUntilItIsApplied) {
  this->config().setup();
  ASSERT_EQ(this->update(R"({"network_type":"auto"})"), nullptr);
  EXPECT_TRUE(this->config().reboot_required());

  // The mode and the network list are what apply_mode() puts into effect.
  this->config().apply_mode();
  EXPECT_FALSE(this->config().reboot_required());
}

TEST_F(Runtime, AnAddressChangeStaysPendingEvenAfterApplying) {
  this->config().setup();
  ASSERT_EQ(this->update(R"({"ethernet":{"use_dhcp":false,"manual_ip":{"static_ip":"10.0.0.7",)"
                         R"("gateway":"10.0.0.1","subnet":"255.255.255.0"}}})"),
            nullptr);
  EXPECT_TRUE(this->config().reboot_required());
  this->config().apply_mode();
  EXPECT_TRUE(this->config().reboot_required());
}

TEST_F(Runtime, TheModeChosenOnTheDisplayRaisesNoRebootBanner) {
  this->config().setup();
  this->config().apply_mode();
  this->config().set_network_type_from_select(2);
  EXPECT_EQ(this->settings().network_type(), NetworkType::WIFI);
  EXPECT_FALSE(this->config().reboot_required());
}

TEST_F(Runtime, ARefusedUpdateIsAnsweredAndChangesNothing) {
  this->config().setup();
  EXPECT_STREQ(this->update(R"({"network_type":"satellite"})"), "'network_type' must be 'auto', 'wifi' or 'ethernet'");
  EXPECT_EQ(this->settings().network_type(), NetworkType::ETHERNET);
  EXPECT_FALSE(this->settings().is_dirty());
  EXPECT_FALSE(this->config().reboot_required());
}

TEST_F(Runtime, TheApiReadsTheRecordThroughTheComponent) {
  ASSERT_TRUE(this->load(R"({"wifi_networks":[{"ssid":"home","password":"s3cret00"}]})"));
  JsonDocument doc;
  this->config().write_public_json(doc.to<JsonObject>());
  std::string body;
  serializeJson(doc, body);
  EXPECT_EQ(body.find("s3cret00"), std::string::npos) << body;
  EXPECT_NE(body.find("has_password"), std::string::npos) << body;
  EXPECT_EQ(this->config().settings(), &this->settings());
}

}  // namespace esphome::network_config::testing
