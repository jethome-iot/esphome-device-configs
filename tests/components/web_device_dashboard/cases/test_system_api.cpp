#include <cctype>
#include "common.h"

namespace esphome::web_device_dashboard::testing {

static const char *const REBOOT = "/api/device/system/reboot";
static const char *const FACTORY_RESET = "/api/device/system/factory-reset";
static const char *const ROLLBACK = "/api/device/system/rollback";
static const char *const CAPABILITIES = "/api/device/capabilities";

// A second slot to roll back to, as the ESP32 build would have read it off the partition.
static RollbackTarget other_slot() {
  RollbackTarget target;
  target.partition = "app1";
  target.version = "2026.8.1";
  target.project_name = "jethome.jxd-r6-e1eth-lcd";
  return target;
}

// --- capabilities ---

TEST_F(Dashboard, CapabilitiesReportsWhatThisFirmwareHas) {
  Reply reply = this->get(CAPABILITIES);
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.type, "application/json");
  EXPECT_TRUE(reply["reboot"].as<bool>());
  EXPECT_TRUE(reply["factory_reset"]["clears_storage"].as<bool>());
  EXPECT_EQ(reply["storage"]["type"].as<std::string>(), "Directory");
  EXPECT_EQ(reply["storage"]["base_path"].as<std::string>(), ".storage");
  EXPECT_TRUE(reply["storage"]["mounted"].as<bool>());
  // How full the mount is belongs to the file API's own `info`: that is live, and this route
  // is read once.
  for (const char *key : {"total_bytes", "used_bytes", "free_bytes"})
    EXPECT_TRUE(reply["storage"][key].isUnbound()) << key;
  EXPECT_TRUE(reply["board_info"].as<bool>());
}

TEST_F(Dashboard, CapabilitiesListsTheSettingsTypesTheStoreHolds) {
  Reply reply = this->get(CAPABILITIES);
  std::vector<std::string> types;
  for (JsonVariant type : reply["entity_settings"]["types"].as<JsonArray>())
    types.emplace_back(type.as<std::string>());
  EXPECT_EQ(types, (std::vector<std::string>{"switch", "binary_sensor", "test"}));
}

TEST_F(Dashboard, CapabilitiesNamesTheScreensThisFirmwareServes) {
  // Absent until the firmware has the components that serve them.
  Reply without = this->get(CAPABILITIES);
  EXPECT_TRUE(without["files"].isUnbound());
  EXPECT_TRUE(without["automations"].isUnbound());

  this->dashboard->set_files_url_prefix("/files");
  this->dashboard->set_automations_url_prefix("/automation-editor");
  Reply with = this->get(CAPABILITIES);
  EXPECT_EQ(with["files"]["url_prefix"].as<std::string>(), "/files");
  EXPECT_EQ(with["automations"]["url_prefix"].as<std::string>(), "/automation-editor");
}

TEST_F(Dashboard, CapabilitiesSaysWhenTheStorageDidNotMount) {
  // The Files screen then has nothing to show, and the page has to be able to tell.
  dir_storage::DirStorage unmounted;
  unmounted.set_base_path("/nonexistent/nowhere");
  unmounted.setup();
  ASSERT_FALSE(unmounted.is_mounted());
  this->dashboard->set_storage(&unmounted);
  Reply reply = this->get(CAPABILITIES);
  EXPECT_FALSE(reply["storage"]["mounted"].as<bool>());
  EXPECT_TRUE(reply["factory_reset"]["clears_storage"].as<bool>());
}

TEST_F(Dashboard, CapabilitiesSaysAFactoryResetKeepsTheFilesWithoutAStorage) {
  this->dashboard->set_storage(nullptr);
  Reply reply = this->get(CAPABILITIES);
  EXPECT_FALSE(reply["factory_reset"]["clears_storage"].as<bool>());
  EXPECT_TRUE(reply["storage"].isUnbound());
}

TEST_F(Dashboard, CapabilitiesDescribesTheSlotARollbackWouldBoot) {
  this->dashboard->stub_rollback = true;
  this->dashboard->rollback = other_slot();
  Reply reply = this->get(CAPABILITIES);
  EXPECT_EQ(reply["rollback"]["partition"].as<std::string>(), "app1");
  EXPECT_EQ(reply["rollback"]["version"].as<std::string>(), "2026.8.1");
  EXPECT_EQ(reply["rollback"]["project_name"].as<std::string>(), "jethome.jxd-r6-e1eth-lcd");
}

TEST_F(Dashboard, CapabilitiesLeavesRollbackOutWhenThereIsNoOtherSlot) {
  // The build's own answer, which off ESP32 is "no second app slot at all".
  Reply reply = this->get(CAPABILITIES);
  EXPECT_TRUE(reply["rollback"].isUnbound());
}

// --- the confirmation every system action takes ---

TEST_F(Dashboard, SystemActionsAnswerPostOnly) {
  for (const char *url : {REBOOT, FACTORY_RESET, ROLLBACK}) {
    LogCapture::instance().clear();
    Reply reply = this->get(url);
    EXPECT_EQ(reply.code, 405) << url;
    EXPECT_EQ(reply.error(), "Method not allowed") << url;
    EXPECT_TRUE(LogCapture::instance().has_warning("allowed: POST")) << url;
  }
  EXPECT_EQ(this->dashboard->restarts, 0);
}

// text/plain is the only one of the three form encodings the server hands on as a raw body, so
// it is the one a cross-site form would arrive as. The type check refuses it before the
// confirmation is even read, and no HTML form can send the type that passes.
TEST_F(Dashboard, SystemActionsRefuseABodyThatDoesNotSayItIsJson) {
  this->dashboard->stub_rollback = true;
  this->dashboard->rollback = other_slot();
  for (const char *url : {REBOOT, FACTORY_RESET, ROLLBACK}) {
    for (const char *type : {"text/plain", "text/plain;charset=UTF-8", "", "application/x-www-form-urlencoded"}) {
      Reply reply = this->call(HTTP_POST, url, this->confirmation(), 512, type);
      EXPECT_EQ(reply.code, 415) << url << " " << type;
      EXPECT_EQ(reply.error(), "Expected Content-Type: application/json") << url << " " << type;
    }
  }
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(this->storage.format_requests, 0);
  EXPECT_EQ(this->dashboard->rollbacks, 0);
}

// --- what a page on another site may do with the browser's cached credentials ---

TEST_F(Dashboard, SystemActionsRefuseAPageOnAnotherSite) {
  this->dashboard->stub_rollback = true;
  this->dashboard->rollback = other_slot();
  for (const char *url : {REBOOT, FACTORY_RESET, ROLLBACK}) {
    // A confirmation this device would otherwise accept: the token is no secret, so the refusal
    // has to come from the Origin and not from the body.
    Reply reply = this->call(HTTP_POST, url, this->confirmation(), 512, "application/json", "http://evil.example");
    EXPECT_TRUE(reply.claimed) << url;
    EXPECT_EQ(reply.code, 403) << url;
    EXPECT_EQ(reply.type, "text/plain") << url;
    EXPECT_EQ(reply.body, "Cross-origin request refused") << url;
  }
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(this->storage.format_requests, 0);
  EXPECT_EQ(this->dashboard->rollbacks, 0);
}

TEST_F(Dashboard, SystemActionsServeTheDevicesOwnPage) {
  const std::string origin = std::string("http://") + Dashboard::HOST;
  Reply reply = this->call(HTTP_POST, REBOOT, this->confirmation(), 512, "application/json", origin.c_str());
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 1);
}

TEST_F(Dashboard, SystemActionsNeedAConfirmation) {
  this->dashboard->stub_rollback = true;
  this->dashboard->rollback = other_slot();
  for (const char *url : {REBOOT, FACTORY_RESET, ROLLBACK}) {
    // Only a JSON boolean confirms: a truthy number or the string "true" is a client that
    // does not know the contract, not a confirmation.
    for (const char *body : {"{}", R"({"confirm":false})", R"({"confirm_token":"AA:BB:CC"})", R"({"confirm":1})",
                             R"({"confirm":"true"})", R"({"confirm":null})"}) {
      Reply reply = this->post(url, body);
      EXPECT_EQ(reply.code, 400) << url << " " << body;
      EXPECT_EQ(reply.error(), "'confirm' must be true") << url << " " << body;
    }
  }
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(this->storage.format_requests, 0);
  EXPECT_EQ(this->dashboard->rollbacks, 0);
}

TEST_F(Dashboard, SystemActionsRefuseAnotherDevicesToken) {
  this->dashboard->stub_rollback = true;
  this->dashboard->rollback = other_slot();
  for (const char *url : {REBOOT, FACTORY_RESET, ROLLBACK}) {
    for (const char *token : {"AA:BB:CC", "", "DD:EE"}) {
      Reply reply = this->post(url, this->confirmation(token));
      EXPECT_EQ(reply.code, 403) << url << " " << token;
      EXPECT_EQ(reply.error(), "'confirm_token' must be the last three octets of base_mac_address")
          << url << " " << token;
    }
    // A confirmation with no token at all is the same refusal.
    Reply bare = this->post(url, R"({"confirm":true})");
    EXPECT_EQ(bare.code, 403) << url;
  }
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(this->storage.format_requests, 0);
  EXPECT_EQ(this->dashboard->rollbacks, 0);
}

TEST_F(Dashboard, TheTokenIsTheMacInWhicheverCaseItIsTyped) {
  std::string body = this->confirmation();
  std::transform(body.begin(), body.end(), body.begin(), [](unsigned char c) { return std::tolower(c); });
  Reply reply = this->post(REBOOT, body);
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
}

TEST_F(Dashboard, SystemActionsRefuseABodyThatIsNotAnObject) {
  for (const char *body : {"", "not json", "[1,2]", R"("confirm")"}) {
    Reply reply = this->post(REBOOT, body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), "Invalid JSON") << body;
  }
}

TEST_F(Dashboard, SystemActionsRefuseABodyOverTheCap) {
  this->dashboard->stub_rollback = true;
  this->dashboard->rollback = other_slot();
  const std::string padding(5000, 'x');
  const std::string body = R"({"confirm":true,"confirm_token":"AA:BB:CC","pad":")" + padding + R"("})";
  for (const char *url : {REBOOT, FACTORY_RESET, ROLLBACK}) {
    Reply reply = this->post(url, body);
    EXPECT_EQ(reply.code, 413) << url;
    EXPECT_EQ(reply.error(), "Request body over 4 KiB") << url;
  }
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(this->storage.format_requests, 0);
  EXPECT_EQ(this->dashboard->rollbacks, 0);
}

// --- the actions themselves ---

TEST_F(Dashboard, RebootAnswersBeforeItReboots) {
  Reply reply = this->post(REBOOT, this->confirmation());
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Rebooting");
  // Still serving: the answer has to leave the socket first.
  EXPECT_EQ(this->dashboard->restarts, 0);
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 1);
}

TEST_F(Dashboard, FactoryResetClearsThePreferencesAndAsksForTheWipeThenReboots) {
  ESPPreferenceObject pref = global_preferences->make_preference<uint32_t>(0x5eedU);
  uint32_t saved = 42;
  ASSERT_TRUE(pref.save(&saved));

  Reply reply = this->post(FACTORY_RESET, this->confirmation());
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Factory reset, rebooting");
  EXPECT_EQ(this->storage.format_requests, 0);

  this->loop();
  uint32_t back = 0;
  EXPECT_FALSE(pref.load(&back));
  // Asked for after the preferences went, because clearing them takes all of NVS with it,
  // and the partition is wiped at the next boot.
  EXPECT_EQ(this->storage.format_requests, 1);
  EXPECT_EQ(this->dashboard->restarts, 1);
}

// False is the backend's last word: it could neither record the request nor wipe on the spot,
// so nothing will retry. The reset still finishes, because the preferences are already gone.
TEST_F(Dashboard, AFactoryResetWhoseStorageCouldNotWipeSaysSoAndStillReboots) {
  this->storage.format_result = false;
  Reply reply = this->post(FACTORY_RESET, this->confirmation());
  EXPECT_EQ(reply.code, 200);
  this->loop();
  EXPECT_EQ(this->storage.format_requests, 1);
  EXPECT_TRUE(LogCapture::instance().has_error("The user partition was not wiped and will not be"));
  EXPECT_EQ(this->dashboard->restarts, 1);
}

TEST_F(Dashboard, AFactoryResetOnAFirmwareWithNoStorageJustClearsThePreferences) {
  this->dashboard->set_storage(nullptr);
  Reply reply = this->post(FACTORY_RESET, this->confirmation());
  EXPECT_EQ(reply.code, 200);
  this->loop();
  EXPECT_EQ(this->storage.format_requests, 0);
  EXPECT_EQ(this->dashboard->restarts, 1);
}

TEST_F(Dashboard, RollbackIsUnavailableWithoutASecondSlot) {
  Reply reply = this->post(ROLLBACK, this->confirmation());
  EXPECT_EQ(reply.code, 503);
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "No firmware to roll back to");
  EXPECT_EQ(this->dashboard->rollbacks, 0);
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
}

TEST_F(Dashboard, RollbackReportsItsAvailabilityBeforeItAsksForAConfirmation) {
  // Which firmware is installed is not something the caller can get wrong.
  Reply reply = this->post(ROLLBACK, "{}");
  EXPECT_EQ(reply.code, 503);
  EXPECT_EQ(reply.error(), "No firmware to roll back to");
}

TEST_F(Dashboard, RollbackSelectsTheOtherSlotAndReboots) {
  this->dashboard->stub_rollback = true;
  this->dashboard->rollback = other_slot();
  Reply reply = this->post(ROLLBACK, this->confirmation());
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Rolling back, rebooting");
  // Selected while the request was still open, so a slot that fails to verify is an error the
  // caller sees rather than a device that comes back unchanged.
  EXPECT_EQ(this->dashboard->rollbacks, 1);
  EXPECT_EQ(this->dashboard->restarts, 0);
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 1);
}

TEST_F(Dashboard, RollbackSaysWhyTheSlotCouldNotBeSelected) {
  this->dashboard->stub_rollback = true;
  this->dashboard->rollback = other_slot();
  this->dashboard->rollback_error = "ESP_ERR_OTA_VALIDATE_FAILED";
  Reply reply = this->post(ROLLBACK, this->confirmation());
  EXPECT_EQ(reply.code, 500);
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "ESP_ERR_OTA_VALIDATE_FAILED");
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
}

}  // namespace esphome::web_device_dashboard::testing
