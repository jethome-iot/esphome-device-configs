#include "common.h"

namespace esphome::entity_config::testing {

TEST(BinarySensorRecord, RoundTripsThroughJson) {
  BinarySensorSettingsRecord record;
  EXPECT_TRUE(load(R"({"source_name":"input_1","inverted":true,"label":"Входная дверь"})", record));
  EXPECT_TRUE(record.inverted);
  EXPECT_EQ(record.label, "Входная дверь");
  EXPECT_EQ(record.key(), fnv1_hash("input_1"));
  EXPECT_EQ(dump(record), R"({"source_name":"input_1","inverted":true,"label":"Входная дверь"})");
  BinarySensorSettingsRecord sparse;
  EXPECT_TRUE(load(R"({"source_name":"input_2"})", sparse));
  EXPECT_FALSE(sparse.inverted);
  EXPECT_EQ(sparse.label, "");
  EXPECT_EQ(dump(sparse), R"({"source_name":"input_2","inverted":false,"label":""})");
  EXPECT_FALSE(load(R"({"inverted":true})", sparse));
}

TEST(BinarySensorRecord, AnInvalidLabelInTheFileIsDropped) {
  LogCapture::instance().warnings.clear();
  BinarySensorSettingsRecord record;
  EXPECT_TRUE(load(R"({"source_name":"input_1","inverted":true,"label":"line\nbreak"})", record));
  EXPECT_EQ(record.label, "");
  EXPECT_TRUE(record.inverted);
  EXPECT_TRUE(LogCapture::instance().has("Invalid label for 'input_1'"));
}

class BinarySensorSettings : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_entities();
    LogCapture::instance().warnings.clear();
  }
  void TearDown() override {
    // The filter stays in the input's chain: leave it transparent for the next test.
    settings.set_inverted(&e.in1, false);
    settings.set_inverted(&e.in2, false);
    manager().remove_binding(fnv1_hash("relay_1"));
    manager().remove_binding(fnv1_hash("relay_2"));
    reset_entities();
  }

  BinarySensorSettingsJson settings;
  Entities &e = entities();
};

TEST_F(BinarySensorSettings, AStoredInversionIsAFilterOnTheInput) {
  auto *record = new BinarySensorSettingsRecord();  // NOLINT(cppcoreguidelines-owning-memory)
  EXPECT_TRUE(load(R"({"source_name":"input_1","inverted":true})", *record));
  settings.records().push_back(record);
  auto *ghost = new BinarySensorSettingsRecord();  // NOLINT(cppcoreguidelines-owning-memory)
  EXPECT_TRUE(load(R"({"source_name":"no_such_input","inverted":true})", *ghost));
  settings.records().push_back(ghost);
  settings.apply();
  EXPECT_TRUE(LogCapture::instance().has("Binary sensor not found for source_name 'no_such_input'"));

  e.in1.publish_state(true);
  EXPECT_FALSE(e.in1.state);
  e.in1.publish_state(false);
  EXPECT_TRUE(e.in1.state);
  EXPECT_TRUE(settings.is_inverted(&e.in1));
  EXPECT_FALSE(settings.is_inverted(&e.in2));
}

TEST_F(BinarySensorSettings, AFlipReEmitsTheStateWithoutWaitingForAnEdge) {
  settings.apply();
  e.in2.publish_state(true);
  settings.set_inverted(&e.in2, true);
  EXPECT_FALSE(e.in2.state);
  EXPECT_TRUE(settings.is_dirty());
  settings.set_inverted(&e.in2, false);
  EXPECT_TRUE(e.in2.state);
  // The same value twice changes nothing.
  settings.set_inverted(&e.in2, false);
  EXPECT_TRUE(e.in2.state);
}

TEST_F(BinarySensorSettings, AFlipIsALevelForBindingsNotAnEdge) {
  bindings::BindingsManager &m = manager();
  m.set_binding(fnv1_hash("relay_1"), fnv1_hash("input_1"), bindings::BindingMode::TOGGLE);
  m.set_binding(fnv1_hash("relay_2"), fnv1_hash("input_1"), bindings::BindingMode::FOLLOW);
  settings.apply();

  e.in1.publish_state(true);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_TRUE(e.relay2.state);
  EXPECT_EQ(e.relay1.writes, 1);
  const int follow_writes = e.relay2.writes;

  settings.set_inverted(&e.in1, true);   // the input now reads false
  settings.set_inverted(&e.in1, false);  // and true again: a rising edge, were it one
  EXPECT_TRUE(e.relay1.state);           // toggle did not fire
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_TRUE(e.relay2.state);  // follow tracked the level both ways
  EXPECT_EQ(e.relay2.writes, follow_writes + 2);

  // A real edge afterwards still toggles.
  e.in1.publish_state(false);
  e.in1.publish_state(true);
  EXPECT_FALSE(e.relay1.state);
}

TEST_F(BinarySensorSettings, ARestWriteRefusesAnInvertedThatIsNotABoolean) {
  JsonDocument refused = body(R"({"source_name":"input_1","settings":{"inverted":"yes"}})");
  EXPECT_EQ(settings.update_record(refused.as<JsonObject>()), nullptr);
  EXPECT_FALSE(settings.is_inverted(&e.in1));

  JsonDocument accepted = body(R"({"source_name":"input_1","settings":{"inverted":true}})");
  ASSERT_NE(settings.update_record(accepted.as<JsonObject>()), nullptr);
  EXPECT_TRUE(settings.is_inverted(&e.in1));
}

// --- labels ---

TEST_F(BinarySensorSettings, ALabelIsShownInPlaceOfTheName) {
  EXPECT_EQ(settings.display_name(&e.in1), "Input 1");
  JsonDocument labelled = body(R"({"source_name":"input_1","settings":{"label":" Входная дверь  "}})");
  ASSERT_NE(settings.update_record(labelled.as<JsonObject>()), nullptr);
  EXPECT_EQ(settings.display_name(&e.in1), "Входная дверь");
  EXPECT_EQ(settings.get_label("input_1"), "Входная дверь");
  EXPECT_EQ(settings.display_name(&e.in2), "Input 2");
  EXPECT_EQ(settings.get_label("no_such_input"), "");

  JsonDocument cleared = body(R"({"source_name":"input_1","settings":{"label":"   "}})");
  ASSERT_NE(settings.update_record(cleared.as<JsonObject>()), nullptr);
  EXPECT_EQ(settings.display_name(&e.in1), "Input 1");
}

// `inverted` keeps its own rule, left out is false; the label is kept.
TEST_F(BinarySensorSettings, ALabelLeftOutKeepsItsValueWhileInvertedLeftOutIsFalse) {
  JsonDocument first = body(R"({"source_name":"input_1","settings":{"inverted":true,"label":"Door"}})");
  ASSERT_NE(settings.update_record(first.as<JsonObject>()), nullptr);
  EXPECT_TRUE(settings.is_inverted(&e.in1));

  JsonDocument without = body(R"({"source_name":"input_1","settings":{}})");
  ASSERT_NE(settings.update_record(without.as<JsonObject>()), nullptr);
  EXPECT_EQ(settings.get_label("input_1"), "Door");
  EXPECT_FALSE(settings.is_inverted(&e.in1));

  JsonDocument inverted_only = body(R"({"source_name":"input_1","settings":{"inverted":true}})");
  ASSERT_NE(settings.update_record(inverted_only.as<JsonObject>()), nullptr);
  EXPECT_EQ(settings.get_label("input_1"), "Door");
  EXPECT_TRUE(settings.is_inverted(&e.in1));

  JsonDocument label_only = body(R"({"source_name":"input_1","settings":{"label":"Gate"}})");
  ASSERT_NE(settings.update_record(label_only.as<JsonObject>()), nullptr);
  EXPECT_EQ(settings.get_label("input_1"), "Gate");
  EXPECT_FALSE(settings.is_inverted(&e.in1));
}

TEST_F(BinarySensorSettings, ABadLabelRefusesTheWholeWrite) {
  JsonDocument first = body(R"({"source_name":"input_1","settings":{"label":"Door"}})");
  ASSERT_NE(settings.update_record(first.as<JsonObject>()), nullptr);

  const std::string head = R"({"source_name":"input_1","settings":{"inverted":true,"label":)";
  for (const std::string label : {
           std::string(R"("Twenty-five characters!!!")"),
           std::string(R"("a\u0000b")"),
           std::string("\"\xFF\""),
           std::string("[\"Door\"]"),
       }) {
    JsonDocument refused = body((head + label + "}}").c_str());
    EXPECT_EQ(settings.update_record(refused.as<JsonObject>()), nullptr) << label;
  }
  EXPECT_EQ(settings.get_label("input_1"), "Door");
  EXPECT_FALSE(settings.is_inverted(&e.in1));
}

TEST_F(BinarySensorSettings, TheMetaOffersALabelFirst) {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  settings.write_settings_meta(obj);
  EXPECT_EQ(obj["label"]["type"].as<std::string>(), "string");
  EXPECT_EQ(obj["label"]["max_length"].as<int>(), 24);
  EXPECT_EQ(std::string(obj.begin()->key().c_str()), "label");
  EXPECT_EQ(obj["inverted"]["type"].as<std::string>(), "boolean");
}

}  // namespace esphome::entity_config::testing
