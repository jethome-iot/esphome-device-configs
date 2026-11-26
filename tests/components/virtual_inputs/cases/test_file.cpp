// The file's format: what a record must carry, what is skipped, and what the device writes.
#include "common.h"

namespace esphome::virtual_inputs::testing {

namespace {

JsonDocument parse(const char *json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok)
    ADD_FAILURE() << "test JSON does not parse: " << json;
  return doc;
}

InputFile file_of(size_t slots) {
  InputFile file("virtual_inputs");
  file.set_slots(slots);
  return file;
}

std::string written(InputFile &file) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  file.write_json(root, 1);
  std::string text;
  serializeJson(doc, text);
  return text;
}

}  // namespace

TEST(ParseDelay, AWholeNumberOfSecondsUpToADay) {
  for (const char *good : {"0", "1", "600", "86400"}) {
    JsonDocument doc = parse(good);
    uint32_t seconds = 99;
    EXPECT_TRUE(parse_delay(doc.as<JsonVariantConst>(), seconds)) << good;
    EXPECT_EQ(seconds, static_cast<uint32_t>(std::stoul(good)));
  }
  for (const char *bad : {"86401", "-1", "1.5", "600.0", "\"600\"", "true", "null", "[]", "1e10"}) {
    JsonDocument doc = parse(bad);
    uint32_t seconds = 99;
    EXPECT_FALSE(parse_delay(doc.as<JsonVariantConst>(), seconds)) << bad;
    EXPECT_EQ(seconds, 99u) << bad;
  }
}

TEST(InputFile, RoundTripsInSlotOrder) {
  InputFile file = file_of(4);
  JsonDocument doc = parse(R"({"version":1,"records":[{"slot":3,"source":"input_2","on_delay":5,"off_delay":600},)"
                           R"({"slot":1,"source":"input_1"}]})");
  ASSERT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
  EXPECT_EQ(file.size(), 2u);
  EXPECT_EQ(file.table()[0], follow_config("input_1", 0, 0));
  EXPECT_FALSE(file.table()[1].configured());
  EXPECT_EQ(file.table()[2], follow_config("input_2", 5, 600));
  EXPECT_EQ(written(file), R"({"records":[{"slot":1,"source":"input_1","on_delay":0,"off_delay":0},)"
                           R"({"slot":3,"source":"input_2","on_delay":5,"off_delay":600}]})");
}

TEST(InputFile, NoRecordsArrayIsAFileThatDidNotLoad) {
  for (const char *bad : {R"({"version":1})", R"({"records":{}})", R"({"records":"none"})"}) {
    InputFile file = file_of(2);
    JsonDocument doc = parse(bad);
    log().clear();
    EXPECT_FALSE(file.parse_json(doc.as<JsonObject>(), 1)) << bad;
    EXPECT_TRUE(log().has_error("'records' must be an array"));
  }
}

TEST(InputFile, ARecordItCannotUseIsSkippedAndTheRestLoads) {
  InputFile file = file_of(3);
  JsonDocument doc = parse(R"({"records":[)"
                           R"({"source":"input_1"},)"
                           R"({"slot":0,"source":"input_1"},)"
                           R"({"slot":"1","source":"input_1"},)"
                           R"({"slot":4,"source":"input_1"},)"
                           R"({"slot":1},)"
                           R"({"slot":1,"source":""},)"
                           R"({"slot":1,"source":7},)"
                           R"({"slot":1,"source":"input_1","on_delay":-1},)"
                           R"({"slot":1,"source":"input_1","off_delay":86401},)"
                           R"({"slot":1,"source":"input_1","off_delay":"600"},)"
                           R"({"slot":1,"source":"input_1","on_delay":0.5},)"
                           R"({"slot":2,"source":"input_2","off_delay":30}]})");
  log().clear();
  ASSERT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
  EXPECT_FALSE(file.table()[0].configured());
  EXPECT_EQ(file.table()[1], follow_config("input_2", 0, 30));
  EXPECT_FALSE(file.table()[2].configured());
  EXPECT_TRUE(log().has_warning("A record without a valid slot, skipping"));
  EXPECT_TRUE(log().has_warning("Slot 4 is past max_inputs, skipping"));
  EXPECT_TRUE(log().has_warning("Slot 1: the source is not an object id, skipping"));
  EXPECT_TRUE(log().has_warning("Slot 1: a delay is not a whole number of seconds up to 86400, skipping"));
}

TEST(InputFile, ASourceAsLongAsAnObjectIdCanBeIsSkipped) {
  InputFile file = file_of(1);
  const std::string longest(OBJECT_ID_MAX_LEN - 1, 'a');
  const std::string too_long(OBJECT_ID_MAX_LEN, 'a');
  JsonDocument doc;
  doc["records"][0]["slot"] = 1;
  doc["records"][0]["source"] = longest;
  ASSERT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
  EXPECT_EQ(file.table()[0].source, longest);
  doc["records"][0]["source"] = too_long;
  ASSERT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
  EXPECT_FALSE(file.table()[0].configured());
}

TEST(InputFile, OfTwoRecordsForOneSlotTheLaterWins) {
  InputFile file = file_of(2);
  JsonDocument doc = parse(R"({"records":[{"slot":1,"source":"input_1"},{"slot":1,"source":"input_2"}]})");
  log().clear();
  ASSERT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
  EXPECT_EQ(file.table()[0].source, "input_2");
  EXPECT_TRUE(log().has_warning("Slot 1 is listed twice, keeping the last record"));
}

TEST(InputFile, ALoadReplacesWhatTheTableHeld) {
  InputFile file = file_of(2);
  file.set_table({follow_config("input_1", 1, 2), follow_config("input_2", 3, 4)});
  JsonDocument doc = parse(R"({"records":[{"slot":2,"source":"input_1"}]})");
  ASSERT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
  EXPECT_FALSE(file.table()[0].configured());
  EXPECT_EQ(file.table()[1], follow_config("input_1", 0, 0));
  file.reset();
  EXPECT_EQ(file.size(), 0u);
  EXPECT_EQ(written(file), R"({"records":[]})");
  EXPECT_STREQ(file.get_key(), "virtual_inputs");
  file.apply();  // the pool applies it, not a keeper: nothing to do
}

}  // namespace esphome::virtual_inputs::testing
