#include <sys/stat.h>
#include <cstring>
#include "common.h"
#include "esphome/core/application.h"

namespace esphome::network_config::testing {

// The drain over ESPHome's own WiFi credential slot, which the host preferences manager provides
// the same way NVS does on the device: one blob per key, read only at its stored length.
class Inbox : public ::testing::Test {
 protected:
  struct Wiring {
    NetworkSettingsJson settings;
    FakeStorage backend;
    config_json::ConfigJsonKeeper keeper;
    TestConfig config{&settings};
  };

  void SetUp() override {
    log().clear();
    reset_preferences();
    // The component outlives the test: its poll is a scheduler entry pointing at it.
    static std::vector<std::unique_ptr<Wiring>> all;
    all.push_back(std::make_unique<Wiring>());
    this->wiring = all.back().get();
    this->settings().set_available_interfaces(true, true);

    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->wiring->backend.path = folder;
    this->keeper().set_storage(&this->wiring->backend);
    this->keeper().set_config_dir("config");
    this->keeper().add_settings(&this->settings());
    config_json::global_config_json_keeper = &this->keeper();
  }

  void TearDown() override {
    this->keeper().cancel_pending_save();
    App.scheduler.cancel_interval(&this->config(), "inbox");
    remove((this->wiring->backend.path + "/config/network.json").c_str());
    rmdir((this->wiring->backend.path + "/config").c_str());
    rmdir(this->wiring->backend.path.c_str());
  }

  NetworkSettingsJson &settings() { return this->wiring->settings; }
  TestConfig &config() { return this->wiring->config; }
  config_json::ConfigJsonKeeper &keeper() { return this->wiring->keeper; }

  bool load(const std::string &text) {
    JsonDocument doc;
    if (deserializeJson(doc, text) != DeserializationError::Ok)
      return false;
    JsonObject root = doc.as<JsonObject>();
    return this->settings().parse_json(root, 1U);
  }

  // What save_wifi_sta() leaves behind: the record, at its own length, under one of the two keys.
  static void write_inbox(uint32_t key, const char *ssid, const char *password) {
    InboxRecord record{};
    strncpy(record.ssid, ssid, sizeof(record.ssid) - 1);
    strncpy(record.password, password, sizeof(record.password) - 1);
    auto pref = global_preferences->make_preference<InboxRecord>(key, true);
    ASSERT_TRUE(pref.save(&record));
    ASSERT_TRUE(global_preferences->sync());
  }

  // What WiFiComponent::start() would find there.
  static bool inbox_readable(uint32_t key) {
    InboxRecord record{};
    return global_preferences->load_from_key(key, reinterpret_cast<uint8_t *>(&record), sizeof(record));
  }

  Wiring *wiring{nullptr};
};

TEST_F(Inbox, TakesOverWhatTheCaptivePortalLeftAndClearsTheSlot) {
  write_inbox(INBOX_LEGACY_KEY, "home", "s3cret00");
  this->config().setup();

  ASSERT_EQ(this->settings().networks().size(), 1U);
  EXPECT_EQ(this->settings().networks()[0].ssid, "home");
  EXPECT_EQ(this->settings().networks()[0].password, "s3cret00");
  EXPECT_FALSE(this->settings().is_dirty());
  EXPECT_FALSE(inbox_readable(INBOX_LEGACY_KEY));
}

TEST_F(Inbox, ReadsTheConfigVersionKeyAsWell) {
  write_inbox(inbox_config_key(), "shop", "");
  this->config().setup();

  ASSERT_EQ(this->settings().networks().size(), 1U);
  EXPECT_EQ(this->settings().networks()[0].ssid, "shop");
  EXPECT_FALSE(inbox_readable(inbox_config_key()));
}

TEST_F(Inbox, TheNewestCredentialsBecomeThePrimaryNetwork) {
  write_inbox(INBOX_LEGACY_KEY, "old", "");
  write_inbox(inbox_config_key(), "new", "");
  this->config().setup();

  ASSERT_EQ(this->settings().networks().size(), 2U);
  EXPECT_EQ(this->settings().networks()[0].ssid, "new");
  EXPECT_FALSE(inbox_readable(INBOX_LEGACY_KEY));
  EXPECT_FALSE(inbox_readable(inbox_config_key()));
}

// The record today's "Reset WiFi creds" writes: upstream keeps it, matches it against any open
// or hidden AP and waits out ap_timeout before the provisioning AP comes up.
TEST_F(Inbox, AnEmptySsidIsNeverAdoptedAndTheSlotIsCleared) {
  write_inbox(INBOX_LEGACY_KEY, "", "");
  this->config().setup();

  EXPECT_TRUE(this->settings().networks().empty());
  EXPECT_FALSE(this->settings().is_dirty());
  EXPECT_FALSE(inbox_readable(INBOX_LEGACY_KEY));
}

TEST_F(Inbox, ANetworkAlreadyStoredIsLeftInPlaceAndTheSlotIsStillCleared) {
  ASSERT_TRUE(this->load(R"({"wifi_networks":[{"ssid":"home","password":"s3cret00"}]})"));
  write_inbox(INBOX_LEGACY_KEY, "home", "s3cret00");
  this->config().setup();

  ASSERT_EQ(this->settings().networks().size(), 1U);
  EXPECT_FALSE(this->settings().is_dirty());
  EXPECT_FALSE(inbox_readable(INBOX_LEGACY_KEY));
}

TEST_F(Inbox, CredentialsSavedDuringTheSessionAreTakenOverToo) {
  this->config().setup();
  ASSERT_TRUE(this->settings().networks().empty());

  write_inbox(inbox_config_key(), "shop", "s3cret00");
  this->config().drain_inbox_();

  ASSERT_EQ(this->settings().networks().size(), 1U);
  EXPECT_EQ(this->settings().networks()[0].ssid, "shop");
  EXPECT_FALSE(this->settings().is_dirty());
  EXPECT_FALSE(inbox_readable(inbox_config_key()));
}

TEST_F(Inbox, ThePollIsArmedForTheRestOfTheSession) {
  this->config().setup();
  EXPECT_TRUE(App.scheduler.cancel_interval(&this->config(), "inbox"));
}

TEST_F(Inbox, ARecordThatCouldNotBeReadKeepsTheSlot) {
  ASSERT_FALSE(this->load(R"({"nothing":"we know"})"));
  ASSERT_TRUE(this->settings().is_broken());
  write_inbox(INBOX_LEGACY_KEY, "home", "s3cret00");
  this->config().setup();

  EXPECT_TRUE(this->settings().networks().empty());
  EXPECT_FALSE(this->settings().is_dirty());
  EXPECT_TRUE(inbox_readable(INBOX_LEGACY_KEY));
}

TEST_F(Inbox, CredentialsThatCouldNotBeSavedKeepTheSlot) {
  config_json::global_config_json_keeper = nullptr;
  write_inbox(INBOX_LEGACY_KEY, "home", "s3cret00");
  this->config().setup();

  EXPECT_TRUE(this->settings().is_dirty());
  EXPECT_TRUE(inbox_readable(INBOX_LEGACY_KEY));
  EXPECT_TRUE(log().has_error("not on flash yet"));
}

}  // namespace esphome::network_config::testing
