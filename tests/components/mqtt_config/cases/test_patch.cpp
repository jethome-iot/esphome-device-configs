#include "common.h"

namespace esphome::mqtt_config::testing {

// parse_patch() on a body the route already knows is a JSON object; "" when it parsed.
static std::string parse(const char *json, MqttPatch &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonObject>()) {
    ADD_FAILURE() << "test JSON is not an object: " << json;
    return "<bad test JSON>";
  }
  return parse_patch(doc.as<JsonObjectConst>(), out);
}
static std::string parse(const char *json) {
  MqttPatch out;
  return parse(json, out);
}

TEST(MqttPatchParse, AnEmptyBodyIsAnEmptyPatch) {
  MqttPatch patch;
  EXPECT_EQ(parse("{}", patch), "");
  EXPECT_FALSE(patch.enabled.has_value());
  EXPECT_FALSE(patch.port.has_value());
  EXPECT_FALSE(patch.broker.has_value());
}

TEST(MqttPatchParse, ReadsEveryKey) {
  MqttPatch patch;
  ASSERT_EQ(parse(R"({"enabled":true,"discovery":false,"port":1884,"broker":"b","username":"u",)"
                  R"("password":"p","client_id":"c","topic_prefix":"t"})",
                  patch),
            "");
  EXPECT_EQ(patch.enabled, true);
  EXPECT_EQ(patch.discovery, false);
  EXPECT_EQ(patch.port, 1884);
  EXPECT_EQ(patch.broker, std::string("b"));
  EXPECT_EQ(patch.username, std::string("u"));
  EXPECT_EQ(patch.password, std::string("p"));
  EXPECT_EQ(patch.client_id, std::string("c"));
  EXPECT_EQ(patch.topic_prefix, std::string("t"));
}

// The messages are the dashboard's too (client/device/mqttRules.ts, MQTT_PATCH_MESSAGES).
TEST(MqttPatchParse, EachWrongTypeHasItsMessage) {
  EXPECT_EQ(parse(R"({"enabled":1})"), "'enabled' must be true or false");
  EXPECT_EQ(parse(R"({"enabled":"true"})"), "'enabled' must be true or false");
  EXPECT_EQ(parse(R"({"discovery":null})"), "'discovery' must be true or false");
  for (const char *key : {"broker", "username", "password", "client_id", "topic_prefix"}) {
    for (const char *value : {"1", "true", "null", "[]", "{}"}) {
      const std::string body = std::string("{\"") + key + "\":" + value + "}";
      EXPECT_EQ(parse(body.c_str()), std::string("'") + key + "' must be a string") << body;
    }
  }
}

// A whole number of any size parses, so validate() can answer with the range; only what is
// not a whole number at all gets this message.
TEST(MqttPatchParse, APortThatIsNotAWholeNumber) {
  for (const char *value : {"1.5", "\"1883\"", "true", "null", "[1883]", "1e30", "99999999999999999999"}) {
    const std::string body = std::string(R"({"port":)") + value + "}";
    EXPECT_EQ(parse(body.c_str()), "'port' must be a whole number") << body;
  }
}

TEST(MqttPatchParse, APortOutOfRangeParsesWithoutWrapping) {
  MqttPatch patch;
  ASSERT_EQ(parse(R"({"port":70000})", patch), "");
  EXPECT_EQ(patch.port, 70000);
  ASSERT_EQ(parse(R"({"port":-1})", patch), "");
  EXPECT_EQ(patch.port, -1);
  ASSERT_EQ(parse(R"({"port":9223372036854775807})", patch), "");
  EXPECT_EQ(patch.port, INT64_MAX);
}

TEST(MqttPatchParse, AnUnknownKeyIsRefusedByName) {
  EXPECT_EQ(parse(R"({"host":"b"})"), "'host' is not an MQTT setting");
  EXPECT_EQ(parse(R"({"password_set":true})"), "'password_set' is not an MQTT setting");
}

// Keys in the body's order: the first bad one is the answer.
TEST(MqttPatchParse, TheFirstBadKeyIsTheAnswer) {
  EXPECT_EQ(parse(R"({"port":"x","host":1})"), "'port' must be a whole number");
  EXPECT_EQ(parse(R"({"host":1,"port":"x"})"), "'host' is not an MQTT setting");
  EXPECT_EQ(parse(R"({"enabled":true,"broker":1,"discovery":2})"), "'broker' must be a string");
}

TEST(MqttPatchParse, AStringKeepsANulForValidateToRefuse) {
  MqttPatch patch;
  ASSERT_EQ(parse(R"({"password":"p\u0000w","username":"u"})", patch), "");
  ASSERT_TRUE(patch.password.has_value());
  EXPECT_EQ(*patch.password, std::string("p\0w", 3));

  MqttRecord record;
  patch.apply_to(record);
  EXPECT_STREQ(validate(record, false), "'password' cannot contain a NUL character");
}

TEST(MqttPatchApply, AnAbsentKeyKeepsTheStoredValue) {
  MqttRecord record = enabled_record();
  record.username = "jxd";
  record.password = "pw";
  record.discovery = true;
  MqttPatch patch;
  ASSERT_EQ(parse(R"({"port":1884})", patch), "");
  patch.apply_to(record);
  EXPECT_EQ(record.port, 1884);
  EXPECT_EQ(record.broker, "192.168.1.10");
  EXPECT_EQ(record.username, "jxd");
  EXPECT_EQ(record.password, "pw");
  EXPECT_TRUE(record.enabled);
  EXPECT_TRUE(record.discovery);
}

TEST(MqttPatchApply, AnEmptyPasswordClearsTheStoredOne) {
  MqttRecord record = enabled_record();
  record.username = "jxd";
  record.password = "pw";
  MqttPatch patch;
  ASSERT_EQ(parse(R"({"password":""})", patch), "");
  patch.apply_to(record);
  EXPECT_EQ(record.password, "");
  EXPECT_EQ(record.username, "jxd");
}

TEST(MqttPatchApply, EveryFieldIsCarriedOver) {
  MqttPatch patch;
  ASSERT_EQ(parse(R"({"enabled":true,"discovery":true,"port":8883,"broker":"b","username":"u",)"
                  R"("password":"p","client_id":"c","topic_prefix":"t"})",
                  patch),
            "");
  MqttRecord record;
  patch.apply_to(record);
  EXPECT_TRUE(record.enabled);
  EXPECT_TRUE(record.discovery);
  EXPECT_EQ(record.port, 8883);
  EXPECT_EQ(record.broker, "b");
  EXPECT_EQ(record.username, "u");
  EXPECT_EQ(record.password, "p");
  EXPECT_EQ(record.client_id, "c");
  EXPECT_EQ(record.topic_prefix, "t");
}

}  // namespace esphome::mqtt_config::testing
