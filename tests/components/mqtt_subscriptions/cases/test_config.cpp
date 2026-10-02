#include "common.h"

namespace esphome::mqtt_subscriptions::testing {

static std::string read_body(const std::string &body, SlotConfig &out) {
  JsonDocument doc = parse(body);
  return read_slot(doc.as<JsonObjectConst>(), Source::BODY, out);
}

static std::string read_entry(const std::string &entry, SlotConfig &out) {
  JsonDocument doc = parse(entry);
  return read_slot(doc.as<JsonObjectConst>(), Source::FILE, out);
}

TEST(SlotRecord, OmittedOptionalFieldsTakeTheirDefaults) {
  SlotConfig slot;
  ASSERT_EQ(read_body(R"({"slot":1,"enabled":true,"name":"Outdoor","topic":"z2m/out","kind":"sensor"})", slot), "");
  EXPECT_TRUE(slot.enabled);
  EXPECT_EQ(slot.name, "Outdoor");
  EXPECT_EQ(slot.topic, "z2m/out");
  EXPECT_EQ(slot.kind, SlotKind::SENSOR);
  EXPECT_EQ(slot.json_path, "");
  EXPECT_EQ(slot.unit, "");
  EXPECT_EQ(slot.decimals, 1);
  EXPECT_EQ(slot.payload_on, "ON");
  EXPECT_EQ(slot.payload_off, "OFF");
}

TEST(SlotRecord, TheBodyNeedsEnabledAndKind) {
  SlotConfig slot;
  EXPECT_EQ(read_body(R"({"name":"A","topic":"t","kind":"sensor"})", slot), "'enabled' must be true or false");
  EXPECT_EQ(read_body(R"({"enabled":true,"name":"A","topic":"t"})", slot),
            "'kind' must be 'sensor', 'binary_sensor' or 'text_sensor'");
  // Name and topic are judged by validation, which calls them required.
  EXPECT_EQ(read_body(R"({"enabled":false,"kind":"text_sensor"})", slot), "");
}

TEST(SlotRecord, AWrongTypeIsRefusedInKeyOrder) {
  SlotConfig slot;
  EXPECT_EQ(read_body(R"({"enabled":1,"name":2})", slot), "'enabled' must be true or false");
  EXPECT_EQ(read_body(R"({"name":2,"enabled":1})", slot), "'name' must be a string");
  for (const char *key : {"name", "topic", "json_path", "unit", "payload_on", "payload_off"}) {
    SCOPED_TRACE(key);
    const std::string body = std::string(R"({"enabled":true,"kind":"sensor",")") + key + R"(":null})";
    EXPECT_EQ(read_body(body, slot), std::string("'") + key + "' must be a string");
  }
  EXPECT_EQ(read_body(R"({"enabled":true,"kind":"number"})", slot),
            "'kind' must be 'sensor', 'binary_sensor' or 'text_sensor'");
  EXPECT_EQ(read_body(R"({"enabled":true,"kind":7})", slot),
            "'kind' must be 'sensor', 'binary_sensor' or 'text_sensor'");
  EXPECT_EQ(read_body(R"({"enabled":true,"kind":"sensor","decimals":1.5})", slot),
            "'decimals' must be a whole number from 0 to 4");
  EXPECT_EQ(read_body(R"({"enabled":true,"kind":"sensor","decimals":"1"})", slot),
            "'decimals' must be a whole number from 0 to 4");
}

// Read wide, so validation sees 260 and not the 4 a uint8_t would make of it.
TEST(SlotRecord, DecimalsAreNotWrapped) {
  SlotConfig slot;
  ASSERT_EQ(read_body(R"({"enabled":true,"name":"A","topic":"t","kind":"sensor","decimals":260})", slot), "");
  EXPECT_EQ(slot.decimals, 260);
  EXPECT_EQ(validate_slot(slot, {}), "'decimals' must be a whole number from 0 to 4");
}

TEST(SlotRecord, WhatAGetAnswersMayComeBack) {
  SlotConfig slot;
  EXPECT_EQ(read_body(R"({"slot":2,"enabled":false,"name":"A","topic":"t","kind":"sensor","pending":true,)"
                      R"("status":{"state":"ok"},"entity":null})",
                      slot),
            "");
}

TEST(SlotRecord, AnUnknownKeyIsRefusedInABodyAndSkippedInTheFile) {
  SlotConfig slot;
  EXPECT_EQ(read_body(R"({"enabled":true,"kind":"sensor","qos":1})", slot), "'qos' is not a slot field");
  EXPECT_EQ(read_entry(R"({"slot":1,"enabled":true,"name":"A","topic":"t","kind":"sensor","qos":1})", slot), "");
  // The file's entries may leave out what they do not set.
  EXPECT_EQ(read_entry(R"({"slot":1,"topic":"t"})", slot), "");
  EXPECT_FALSE(slot.enabled);
}

TEST(SlotRecord, TheNameIsTrimmed) {
  SlotConfig slot;
  ASSERT_EQ(read_body(R"({"enabled":true,"name":"  Garage door \t","topic":"t","kind":"sensor"})", slot), "");
  EXPECT_EQ(slot.name, "Garage door");
  EXPECT_EQ(trim_ascii(" \r\n "), "");
}

TEST(SlotRecord, NormalizeResetsWhatTheKindDoesNotUse) {
  SlotConfig sensor = slot_of("A", "t");
  sensor.unit = "°C";
  sensor.decimals = 2;
  sensor.payload_on = "open";
  sensor.normalize();
  EXPECT_EQ(sensor.unit, "°C");
  EXPECT_EQ(sensor.decimals, 2);
  EXPECT_EQ(sensor.payload_on, "ON");

  SlotConfig binary = slot_of("A", "t", SlotKind::BINARY_SENSOR);
  binary.unit = "°C";
  binary.decimals = 3;
  binary.payload_on = "open";
  binary.payload_off = "closed";
  binary.json_path = "contact";
  binary.normalize();
  EXPECT_EQ(binary.unit, "");
  EXPECT_EQ(binary.decimals, 1);
  EXPECT_EQ(binary.payload_on, "open");
  EXPECT_EQ(binary.payload_off, "closed");
  EXPECT_EQ(binary.json_path, "contact");

  SlotConfig text = slot_of("A", "t", SlotKind::TEXT_SENSOR);
  text.unit = "%";
  text.payload_off = "x";
  text.normalize();
  EXPECT_EQ(text.unit, "");
  EXPECT_EQ(text.payload_off, "OFF");

  // An empty slot is all defaults, so equality is plain field equality.
  SlotConfig empty = slot_of("A", "");
  empty.normalize();
  EXPECT_EQ(empty, SlotConfig{});
  EXPECT_FALSE(empty.enabled);
}

TEST(SlotRecord, EqualityIsEveryField) {
  const SlotConfig base = slot_of("A", "t", SlotKind::BINARY_SENSOR);
  EXPECT_EQ(base, base);
  std::vector<std::function<void(SlotConfig &)>> changes = {
      [](SlotConfig &s) { s.enabled = false; },   [](SlotConfig &s) { s.name = "B"; },
      [](SlotConfig &s) { s.topic = "u"; },       [](SlotConfig &s) { s.kind = SlotKind::SENSOR; },
      [](SlotConfig &s) { s.json_path = "a"; },   [](SlotConfig &s) { s.unit = "%"; },
      [](SlotConfig &s) { s.decimals = 2; },      [](SlotConfig &s) { s.payload_on = "1"; },
      [](SlotConfig &s) { s.payload_off = "0"; },
  };
  for (size_t i = 0; i < changes.size(); i++) {
    SCOPED_TRACE(i);
    SlotConfig other = base;
    changes[i](other);
    EXPECT_NE(base, other);
  }
}

TEST(SlotRecord, ARoundTripKeepsEveryField) {
  SlotConfig slot = slot_of("Garage door", "zigbee2mqtt/garage", SlotKind::BINARY_SENSOR);
  slot.json_path = "contact";
  slot.payload_on = "false";
  slot.payload_off = "true";
  JsonDocument doc;
  slot.to_json(doc.to<JsonObject>());
  SlotConfig back;
  ASSERT_EQ(read_slot(doc.as<JsonObjectConst>(), Source::BODY, back), "");
  EXPECT_EQ(back, slot);
  EXPECT_EQ(doc["kind"], "binary_sensor");
}

TEST(SlotFileFormat, OnlyNonEmptySlotsAreWrittenWithEveryField) {
  std::vector<SlotConfig> slots(4);
  slots[2] = slot_of("Garage", "g", SlotKind::TEXT_SENSOR);
  JsonDocument doc = parse(serialize_file(slots));
  EXPECT_EQ(doc["version"], 1);
  ASSERT_EQ(doc["slots"].size(), 1u);
  JsonObject entry = doc["slots"][0];
  EXPECT_EQ(entry["slot"], 3);
  for (const char *key :
       {"enabled", "name", "topic", "kind", "json_path", "unit", "decimals", "payload_on", "payload_off"}) {
    SCOPED_TRACE(key);
    EXPECT_FALSE(entry[key].isNull());
  }
}

TEST(SlotFileFormat, ItReadsBackWhatItWrote) {
  std::vector<SlotConfig> slots(4);
  slots[0] = slot_of("Outdoor", "z2m/out");
  slots[0].unit = "°C";
  slots[0].json_path = "temperature";
  slots[3] = slot_of("Door", "z2m/door", SlotKind::BINARY_SENSOR);
  const std::string text = serialize_file(slots);
  const SlotFile file = parse_file(text.data(), text.size(), 4);
  EXPECT_EQ(file.status, SlotFile::Status::OK);
  EXPECT_EQ(file.slots, slots);
}

TEST(SlotFileFormat, WhatCannotBeReadIsUnreadable) {
  for (const char *text : {"", "not json", "[]", "{}", R"({"version":"1","slots":[]})", R"({"version":0,"slots":[]})",
                           R"({"version":1})", R"({"version":1,"slots":{}})"}) {
    SCOPED_TRACE(text);
    const SlotFile file = parse_file(text, std::strlen(text), 4);
    EXPECT_EQ(file.status, SlotFile::Status::UNREADABLE);
    EXPECT_EQ(file.slots.size(), 4u);
  }
  const std::string huge(FILE_MAX + 1, ' ');
  EXPECT_EQ(parse_file(huge.data(), huge.size(), 4).status, SlotFile::Status::UNREADABLE);
}

// Running out of memory says nothing about the file, so it is no reason to set it aside.
TEST(SlotFileFormat, OnlyAFileThatParsedBadlyIsUnreadable) {
  EXPECT_EQ(parse_failure_status(DeserializationError::NoMemory), SlotFile::Status::FAILED);
  for (auto code : {DeserializationError::EmptyInput, DeserializationError::IncompleteInput,
                    DeserializationError::InvalidInput, DeserializationError::TooDeep}) {
    SCOPED_TRACE(DeserializationError(code).c_str());
    EXPECT_EQ(parse_failure_status(code), SlotFile::Status::UNREADABLE);
  }
}

TEST(SlotFileFormat, ANewerVersionIsNewer) {
  const char *text = R"({"version":2,"slots":[{"slot":1,"enabled":true,"name":"A","topic":"t","kind":"sensor"}]})";
  const SlotFile file = parse_file(text, std::strlen(text), 4);
  EXPECT_EQ(file.status, SlotFile::Status::NEWER);
  EXPECT_TRUE(file.slots[0].empty());
}

TEST(SlotFileFormat, BadEntriesGoAndTheRestStays) {
  const char *text = R"({"version":1,"slots":[
      {"slot":9,"enabled":true,"name":"Past the end","topic":"a","kind":"sensor"},
      {"slot":0,"enabled":true,"name":"Before the start","topic":"a","kind":"sensor"},
      "not an object",
      {"enabled":true,"name":"No number","topic":"a","kind":"sensor"},
      {"slot":2,"enabled":"yes","name":"Wrong type","topic":"a","kind":"sensor"},
      {"slot":3,"enabled":true,"name":"First","topic":"a","kind":"sensor"},
      {"slot":3,"enabled":true,"name":"Second","topic":"b","kind":"sensor"},
      {"slot":4,"enabled":true,"name":"","topic":"a+b","kind":"sensor"}]})";
  const SlotFile file = parse_file(text, std::strlen(text), 4);
  ASSERT_EQ(file.status, SlotFile::Status::OK);
  EXPECT_TRUE(file.slots[0].empty());
  EXPECT_TRUE(file.slots[1].empty());
  EXPECT_EQ(file.slots[2].name, "First");
  // Kept as written: validation at boot keeps it from running and says why.
  EXPECT_EQ(file.slots[3].topic, "a+b");
}

}  // namespace esphome::mqtt_subscriptions::testing
