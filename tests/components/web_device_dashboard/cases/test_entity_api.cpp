#include "common.h"

namespace esphome::web_device_dashboard::testing {

TEST_F(Dashboard, EntitiesListsOneEntryPerEntityWithSettings) {
  Reply reply = this->get("/api/device/entities");
  ASSERT_EQ(reply.code, 200);
  JsonVariant switches = reply["switch"];
  ASSERT_EQ(switches.size(), 1u) << reply.body;  // the internal switch is left out
  EXPECT_EQ(switches[0]["source_name"].as<std::string>(), "relay_1");
  EXPECT_EQ(switches[0]["name"].as<std::string>(), "Relay 1");
  JsonVariant inputs = reply["binary_sensor"];
  ASSERT_EQ(inputs.size(), 1u) << reply.body;
  EXPECT_EQ(inputs[0]["source_name"].as<std::string>(), "in_1");
  EXPECT_EQ(inputs[0]["name"].as<std::string>(), "In 1");
}

TEST_F(Dashboard, EntitiesLeavesOutASettingsTypeItHasNoEntityKindFor) {
  // The index is keyed by the settings types the keeper holds, and it knows two of them.
  store().other.seed("whatever", false);
  Reply reply = this->get("/api/device/entities");
  EXPECT_TRUE(reply["test"].isNull()) << reply.body;
  EXPECT_EQ(reply.json.size(), 2u) << reply.body;
}

TEST_F(Dashboard, EntitiesIsEmptyWithoutAKeeper) {
  config_json::global_config_json_keeper = nullptr;
  Reply reply = this->get("/api/device/entities");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, "{}");
}

TEST_F(Dashboard, EntitySettingsNeedsAType) {
  Reply reply = this->get("/api/device/entity-settings");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Query parameter 'type' is required");
}

TEST_F(Dashboard, EntitySettingsRefusesATypeNobodyRegistered) {
  Reply reply = this->get("/api/device/entity-settings?type=light");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Settings type not found");
}

TEST_F(Dashboard, EntitySettingsReturnsEveryRecordOfAType) {
  ASSERT_EQ(this->post("/api/device/entity-settings", UPDATE_RELAY_1).code, 200);
  Reply reply = this->get("/api/device/entity-settings?type=switch");
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply["type"].as<std::string>(), "switch");
  ASSERT_EQ(reply["records"].size(), 1u) << reply.body;
  EXPECT_EQ(reply["records"][0]["source_name"].as<std::string>(), "relay_1");
  EXPECT_TRUE(reply["records"][0]["inverted"].as<bool>());
}

TEST_F(Dashboard, EntitySettingsCanBeNarrowedToOneEntity) {
  ASSERT_EQ(this->post("/api/device/entity-settings", UPDATE_RELAY_1).code, 200);
  store().sw.seed("relay_2", false);

  Reply all = this->get("/api/device/entity-settings?type=switch");
  EXPECT_EQ(all["records"].size(), 2u);
  Reply one = this->get("/api/device/entity-settings?type=switch&source_name=relay_2");
  ASSERT_EQ(one["records"].size(), 1u) << one.body;
  EXPECT_EQ(one["records"][0]["source_name"].as<std::string>(), "relay_2");
}

TEST_F(Dashboard, EntitySettingsIsUnavailableWithoutAKeeper) {
  config_json::global_config_json_keeper = nullptr;
  Reply read = this->get("/api/device/entity-settings?type=switch");
  EXPECT_EQ(read.code, 503);
  EXPECT_EQ(read.error(), "Config JSON keeper not available");
  Reply write = this->post("/api/device/entity-settings", UPDATE_RELAY_1);
  EXPECT_EQ(write.code, 503);
  EXPECT_EQ(write.error(), "Config JSON keeper not available");
}

TEST_F(Dashboard, APostOfSomethingThatIsNotAnObjectIsRefused) {
  for (const char *body : {"not json", "[]", "\"switch\"", "42"}) {
    Reply reply = this->post("/api/device/entity-settings", body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), "Invalid JSON") << body;
  }
}

TEST_F(Dashboard, APostWithoutATypeIsRefused) {
  for (const char *body : {R"({"source_name":"relay_1"})", R"({"type":"","source_name":"relay_1"})"}) {
    Reply reply = this->post("/api/device/entity-settings", body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), "'type' is required") << body;
  }
}

TEST_F(Dashboard, APostOfATypeNobodyRegisteredIsRefused) {
  Reply reply = this->post("/api/device/entity-settings", R"({"type":"light","source_name":"lamp"})");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Settings type not found");
}

TEST_F(Dashboard, APostTheSettingsTypeRejectsIsRefused) {
  Reply reply = this->post("/api/device/entity-settings", R"({"type":"switch","settings":{"inverted":true}})");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Failed to update settings record");
  EXPECT_EQ(store().sw.report(), "");
}

TEST_F(Dashboard, APostUpdatesTheRecordAndSchedulesTheSave) {
  Reply reply = this->post("/api/device/entity-settings", UPDATE_RELAY_1);
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Settings updated");
  EXPECT_EQ(store().sw.report(), "relay_1=1");
  EXPECT_TRUE(store().keeper.is_save_pending());
}

TEST_F(Dashboard, TheRecordIsAppliedFromTheLoopAndNotFromTheServer) {
  ASSERT_EQ(this->post("/api/device/entity-settings", UPDATE_RELAY_1).code, 200);
  // Entities are driven from the loop task: the answer goes out before anything is applied.
  EXPECT_TRUE(store().sw.applied.empty());
  this->loop();
  EXPECT_EQ(store().sw.applied, std::vector<std::string>{"relay_1"});
}

TEST_F(Dashboard, EachSettingsTypeIsAddressedOnItsOwn) {
  ASSERT_EQ(this->post("/api/device/entity-settings", UPDATE_RELAY_1).code, 200);
  ASSERT_EQ(this->post("/api/device/entity-settings",
                       R"({"type":"binary_sensor","source_name":"in_1","settings":{"inverted":true}})")
                .code,
            200);
  EXPECT_EQ(store().sw.report(), "relay_1=1");
  EXPECT_EQ(store().bs.report(), "in_1=1");
}

TEST_F(Dashboard, ADeleteRemovesTheRecord) {
  ASSERT_EQ(this->post("/api/device/entity-settings", UPDATE_RELAY_1).code, 200);
  this->loop();  // the update is applied before the next request, as it is on a device
  Reply reply = this->post("/api/device/entity-settings", DELETE_RELAY_1);
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Entity was removed");
  EXPECT_EQ(store().sw.report(), "");
}

TEST_F(Dashboard, ADeleteOfSomethingThatIsNotThereIsRefused) {
  Reply reply = this->post("/api/device/entity-settings", DELETE_RELAY_1);
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Cannot delete: record not found");
}

TEST_F(Dashboard, MetaDescribesEverySettingsTypeTheKeeperHolds) {
  Reply reply = this->get("/api/device/entity-settings-meta");
  ASSERT_EQ(reply.code, 200);
  JsonVariant settings = reply["settings"];
  ASSERT_FALSE(settings.isNull()) << reply.body;
  EXPECT_EQ(settings["switch"]["fields"][0].as<std::string>(), "inverted");
  EXPECT_EQ(settings["binary_sensor"]["fields"][0].as<std::string>(), "inverted");
  EXPECT_EQ(settings["test"]["fields"][0].as<std::string>(), "inverted");
}

TEST_F(Dashboard, MetaIsAnEmptyMapWithoutAKeeper) {
  config_json::global_config_json_keeper = nullptr;
  Reply reply = this->get("/api/device/entity-settings-meta");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, R"({"settings":{}})");
}

}  // namespace esphome::web_device_dashboard::testing
