#include "common.h"
#include <cstdio>
#include "esphome/components/automations/automation_config.h"

namespace esphome::web_automation_editor::testing {

TEST_F(Editor, ClaimsOnlyItsRoutes) {
  for (const char *target :
       {"/", "/automation-editor", "/automation-editor/", "/automation-editor/api/", "/automation-editor/api/list/",
        "/automation-editor/api/listing", "/files/list", "/automation-editor/api/nothing"}) {
    AsyncWebServerRequest request(HTTP_GET, target);
    EXPECT_FALSE(this->base.get_server()->dispatch(request)) << target;
  }
  Reply reply = this->get("ping");
  EXPECT_TRUE(reply.claimed);
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, R"({"status":"ok"})");
}

TEST_F(Editor, AnotherPrefixMovesTheRoutes) {
  TestEditor other(&this->base, this->engine);
  other.set_url_prefix("/rules");
  other.setup();
  AsyncWebServerRequest request(HTTP_GET, "/rules/api/ping");
  EXPECT_TRUE(this->base.get_server()->dispatch(request));
  EXPECT_EQ(request.response_code, 200);
}

TEST_F(Editor, MutatingRoutesArePostOnlyAndTheRestGetOnly) {
  for (const char *route : {"save", "delete?id=1", "reboot"}) {
    Reply reply = this->get(route);
    EXPECT_TRUE(reply.claimed) << route;
    EXPECT_EQ(reply.code, 405) << route;
    EXPECT_EQ(reply.error(), "Method not allowed") << route;
  }
  for (const char *route : {"list", "get?id=1", "export", "entities", "schema", "ping"}) {
    Reply reply = this->post(route);
    EXPECT_EQ(reply.code, 405) << route;
  }
  EXPECT_EQ(this->editor->reboots, 0);
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Editor, AnEmptyEngineListsNothing) {
  EXPECT_EQ(this->get("list").body, R"({"automations":[]})");
  EXPECT_EQ(this->get("export").body, R"({"version":1,"automations":[]})");
}

TEST_F(Editor, SaveCreatesARuleTheOtherRoutesThenSee) {
  Reply saved = this->post("save", FAN);
  ASSERT_EQ(saved.code, 200) << saved.body;
  EXPECT_TRUE(saved["success"].as<bool>());
  EXPECT_EQ(saved.message(), "Automation created");
  const uint32_t id = saved["id"];
  ASSERT_GT(id, 0u);

  Reply list = this->get("list");
  JsonArray rows = list["automations"];
  ASSERT_EQ(rows.size(), 1u);
  JsonObject row = rows[0];
  EXPECT_EQ(row["id"].as<uint32_t>(), id);
  EXPECT_EQ(row["name"].as<std::string>(), "Fan \"on\" when hot");
  EXPECT_FALSE(row["enabled"].as<bool>());
  EXPECT_EQ(row["trigger_count"].as<int>(), 1);
  EXPECT_EQ(row["action_count"].as<int>(), 2);
  EXPECT_EQ(row["else_action_count"].as<int>(), 1);
  EXPECT_EQ(row["mode"].as<std::string>(), "restart");

  Reply got = this->get("get?id=" + std::to_string(id));
  ASSERT_EQ(got.code, 200) << got.body;
  EXPECT_EQ(got["id"].as<uint32_t>(), id);
  EXPECT_EQ(got["name"].as<std::string>(), "Fan \"on\" when hot");
  EXPECT_EQ(got["condition"]["object_id"].as<std::string>(), "in_1");
  EXPECT_EQ(got["actions"][1]["delay_ms"].as<int>(), 5000);

  EXPECT_EQ(this->files(), std::vector<std::string>{"fan_on_when_hot.json"});
}

TEST_F(Editor, SaveWithAnIdUpdatesThatRule) {
  const uint32_t id = this->create(PORCH_LIGHT);
  ASSERT_GT(id, 0u);
  std::string renamed = PORCH_LIGHT;
  renamed.replace(1, 0, "\"id\":" + std::to_string(id) + ",");
  renamed.replace(renamed.find("Porch light"), 11, "Hall light");

  Reply reply = this->post("save", renamed);
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.message(), "Automation updated");
  EXPECT_FALSE(reply.json["id"].is<uint32_t>());

  Reply list = this->get("list");
  ASSERT_EQ(list["automations"].size(), 1u);
  EXPECT_EQ(list["automations"][0]["id"].as<uint32_t>(), id);
  EXPECT_EQ(list["automations"][0]["name"].as<std::string>(), "Hall light");
  EXPECT_EQ(this->files(), std::vector<std::string>{"hall_light.json"});
}

TEST_F(Editor, SaveRefusesANameAnotherRuleOwns) {
  ASSERT_GT(this->create(PORCH_LIGHT), 0u);
  std::string clash = PORCH_LIGHT;
  clash.replace(clash.find("Porch light"), 11, "porch-LIGHT");
  Reply reply = this->post("save", clash);
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "An automation named \"porch-LIGHT\" already exists");
  EXPECT_EQ(this->get("list")["automations"].size(), 1u);
}

TEST_F(Editor, SaveRefusesWhatItCannotRead) {
  struct Case {
    const char *body;
    const char *content_type;
    const char *error;
  };
  for (const Case &c : {
           Case{"", "application/json", "Empty request body"},
           // A form is parsed into fields by the server and never reaches the body.
           Case{PORCH_LIGHT, "application/x-www-form-urlencoded", "Empty request body"},
           Case{R"({"name":"Broken","triggers":[)", "application/json", "JSON parse error: IncompleteInput"},
           Case{R"({"name":"Bad","triggers":[{"source":"inupt","type":"press","object_id":"in_1"}],"actions":[]})",
                "application/json", "Failed to parse automation config"},
       }) {
    Reply reply = this->call(HTTP_POST, "save", c.body, c.content_type);
    EXPECT_EQ(reply.code, 400) << c.body;
    EXPECT_EQ(reply.error(), c.error) << c.body;
  }
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Editor, SaveRefusesABodyOverTheFileLimit) {
  std::string padded = PORCH_LIGHT;
  padded.replace(1, 0, "\"note\":\"" + std::string(16384, 'x') + "\",");
  Reply reply = this->post("save", padded);
  EXPECT_EQ(reply.code, 413);
  EXPECT_EQ(reply.error(), "Request body over 16 KiB");
  EXPECT_TRUE(this->files().empty());
  // The refusal leaves nothing behind for the next request.
  EXPECT_GT(this->create(PORCH_LIGHT), 0u);
}

// web_server_idf never reaches handleRequest when the receive fails midway, so the buffer
// outlives that request; the next one must not read it as its own.
TEST_F(Editor, ABodyLeftByAFailedReceiveIsNotTheNextRequests) {
  std::string partial = PORCH_LIGHT;
  AsyncWebServerRequest aborted(HTTP_POST, "/automation-editor/api/save", partial);
  this->editor->handleBody(&aborted, reinterpret_cast<uint8_t *>(&partial[0]), 20, 0, partial.size());

  Reply reply = this->post("save");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Empty request body");
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Editor, DeleteRemovesTheRuleAndItsFile) {
  const uint32_t id = this->create(PORCH_LIGHT);
  ASSERT_GT(id, 0u);
  Reply reply = this->post("delete?id=" + std::to_string(id));
  EXPECT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.message(), "Automation deleted");
  EXPECT_TRUE(this->files().empty());
  EXPECT_EQ(this->get("list").body, R"({"automations":[]})");

  reply = this->post("delete?id=" + std::to_string(id));
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Automation not found");
  reply = this->post("delete");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Missing id parameter");
}

TEST_F(Editor, SaveWithAnUnknownIdIsNotFound) {
  Reply reply = this->post("save", R"({"id":77,"name":"Ghost","triggers":[],"actions":[]})");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Automation not found");
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Editor, GetNeedsAnExistingId) {
  Reply reply = this->get("get");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Missing id parameter");
  reply = this->get("get?id=99");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Automation not found");
}

TEST_F(Editor, ExportHoldsEveryRuleAsGetReturnsIt) {
  const uint32_t porch = this->create(PORCH_LIGHT);
  const uint32_t fan = this->create(FAN);
  ASSERT_GT(porch, 0u);
  ASSERT_GT(fan, 0u);

  Reply exported = this->get("export");
  ASSERT_EQ(exported.code, 200);
  EXPECT_EQ(exported["version"].as<int>(), 1);
  JsonArray rules = exported["automations"];
  ASSERT_EQ(rules.size(), 2u);
  for (JsonObject rule : rules) {
    std::string one;
    serializeJson(rule, one);
    EXPECT_EQ(one, this->get("get?id=" + rule["id"].as<std::string>()).body);
  }
}

TEST_F(Editor, EntitiesListsWhatIsNotInternal) {
  Reply reply = this->get("entities");
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, R"({"binary_sensors":[{"object_id":"in_1","name":"In 1"}],)"
                        R"("sensors":[{"object_id":"temp","name":"Temp","unit":"°C"}],)"
                        R"("switches":[{"object_id":"relay_1","name":"Relay 1"}]})");
}

// The schema is a static string; this keeps every word in it one the engine's parsers take.
TEST_F(Editor, SchemaOffersOnlyWhatTheEngineParses) {
  Reply reply = this->get("schema");
  ASSERT_EQ(reply.code, 200);

  auto parses = [](const std::string &json, auto &config) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok)
      return false;
    JsonObject obj = doc.as<JsonObject>();
    return config.deserialize(obj);
  };
  auto trigger = [&](const std::string &json) {
    automations::TriggerConfig config;
    return parses(json, config);
  };
  auto action = [&](const std::string &json) {
    automations::ActionConfig config;
    return parses(json, config);
  };
  auto condition = [&](const std::string &json) {
    automations::ConditionConfig config;
    return parses(json, config);
  };

  static const char *const THRESHOLDS = R"(,"threshold":1,"min_threshold":1,"max_threshold":2)";
  int words = 0;
  for (JsonObject entry : reply["triggers"].as<JsonArray>()) {
    const std::string type = entry["type"];
    const std::string object = type == "temperature" ? "temp" : type == "switch" ? "relay_1" : "in_1";
    if (entry["subtypes"].size() == 0) {
      const std::string cron = type == "cron" ? R"(,"cron":"0 * * * * *")" : "";
      EXPECT_TRUE(trigger(R"({"source":")" + type + "\"" + cron + "}")) << type;
      words++;
    }
    for (std::string subtype : entry["subtypes"].as<JsonArray>()) {
      EXPECT_TRUE(trigger(R"({"source":")" + type + R"(","type":")" + subtype + R"(","object_id":")" + object + "\"" +
                          THRESHOLDS + "}"))
          << type << "/" << subtype;
      words++;
    }
  }
  for (JsonObject entry : reply["conditions"].as<JsonArray>()) {
    const std::string type = entry["type"];
    if (type == "input") {
      EXPECT_TRUE(condition(R"({"type":"input","object_id":"in_1","state":"true"})"));
    } else if (type == "temperature") {
      for (std::string subtype : entry["subtypes"].as<JsonArray>()) {
        EXPECT_TRUE(condition(R"({"type":"temperature","object_id":"temp","temperature_type":")" + subtype + "\"" +
                              THRESHOLDS + "}"))
            << subtype;
        words++;
      }
    } else {
      EXPECT_TRUE(
          condition(R"({"type":")" + type + R"(","conditions":[{"type":"input","object_id":"in_1","state":"true"}]})"))
          << type;
    }
    words++;
  }
  for (JsonObject entry : reply["actions"].as<JsonArray>()) {
    const std::string type = entry["type"];
    if (entry["subtypes"].size() == 0) {
      EXPECT_TRUE(action(R"({"source":")" + type + R"(","delay_ms":1})")) << type;
      words++;
    }
    for (std::string subtype : entry["subtypes"].as<JsonArray>()) {
      EXPECT_TRUE(action(R"({"source":"switch","type":")" + subtype + R"(","object_id":"relay_1"})")) << subtype;
      words++;
    }
  }
  for (std::string preset : reply["cron_presets"].as<JsonArray>()) {
    EXPECT_TRUE(trigger(R"({"source":"cron","cron":"0 * * * * *","cron_preset":")" + preset + "\"}")) << preset;
    words++;
  }
  // Every word above was checked, not an empty schema.
  EXPECT_EQ(words, 31);
}

TEST_F(Editor, RebootAnswersFirst) {
  Reply reply = this->post("reboot");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Rebooting device...");
  EXPECT_EQ(this->editor->reboots, 1);
}

}  // namespace esphome::web_automation_editor::testing
