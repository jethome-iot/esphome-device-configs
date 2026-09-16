#include "common.h"

namespace esphome::entity_config::testing {

TEST(BinarySensorRecord, RoundTripsThroughJson) {
  BinarySensorSettingsRecord record;
  EXPECT_TRUE(load(R"({"source_name":"input_1","inverted":true})", record));
  EXPECT_TRUE(record.inverted);
  EXPECT_EQ(record.key(), fnv1_hash("input_1"));
  EXPECT_EQ(dump(record), R"({"source_name":"input_1","inverted":true})");
  BinarySensorSettingsRecord sparse;
  EXPECT_TRUE(load(R"({"source_name":"input_2"})", sparse));
  EXPECT_FALSE(sparse.inverted);
  EXPECT_FALSE(load(R"({"inverted":true})", sparse));
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

}  // namespace esphome::entity_config::testing
