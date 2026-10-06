#include "common.h"
#include <map>
#include "esphome/components/switch_hold/switch_hold.h"

namespace esphome::entity_config::testing {

using Field = SwitchSettingsJson::Field;

// Stands in for climate_hub: holds the switches the case puts in the table, by name.
class FakeHolder : public switch_hold::SwitchHolder {
 public:
  std::map<const switch_::Switch *, std::string> held;
  std::string holder_of(const switch_::Switch *sw) const override {
    auto it = this->held.find(sw);
    return it == this->held.end() ? std::string() : it->second;
  }
};

static const char *const HELD_RELAY_1 =
    "\"Relay 1\" is driven by \"Living room\": stop that thermostat to change Inverted";

// A flipped contact under a running thermostat would invert what it does, so Inverted waits
// until the thermostat lets go; every other setting changes as usual.
class HeldRelay : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_entities();
    switch_hold::set_holder(&this->holder);
    this->holder.held[&this->e.relay1] = "Living room";
    this->settings.apply();
  }
  void TearDown() override {
    switch_hold::set_holder(nullptr);
    manager().remove_binding(fnv1_hash("relay_1"));
    reset_entities();
  }
  // What the dashboard's POST does: the hook it calls, then the apply on success.
  bool post(const char *json) {
    JsonDocument doc = body(json);
    if (this->settings.update_record_from_json(doc.as<JsonObject>()) == nullptr)
      return false;
    this->settings.apply();
    return true;
  }

  SwitchSettingsJson settings;
  FakeHolder holder;
  Entities &e = entities();
};

TEST_F(HeldRelay, ARestWriteThatFlipsInvertedIsRefusedNamingTheThermostat) {
  EXPECT_FALSE(this->post(R"({"source_name":"relay_1","settings":{"inverted":true}})"));
  EXPECT_EQ(HELD_RELAY_1, this->settings.conflict());
  EXPECT_FALSE(e.relay1.is_inverted());
  EXPECT_EQ(e.relay1.writes, 0);
  EXPECT_TRUE(this->settings.records().empty()) << "nothing stored either";
}

// The dashboard shows a labelled relay by its label, so its refusal names it so too.
TEST_F(HeldRelay, TheRefusalNamesALabelledRelayByItsLabel) {
  ASSERT_TRUE(this->post(R"({"source_name":"relay_1","settings":{"label":"Котёл"}})"));
  EXPECT_FALSE(this->post(R"({"source_name":"relay_1","settings":{"inverted":true}})"));
  EXPECT_EQ("\"Котёл\" is driven by \"Living room\": stop that thermostat to change Inverted",
            this->settings.conflict());
  EXPECT_EQ(HELD_RELAY_1, inverted_refusal(&e.relay1)) << "with nothing to show, the name";
}

TEST_F(HeldRelay, ARestWriteThatKeepsInvertedChangesTheRest) {
  ASSERT_TRUE(this->post(R"({"source_name":"relay_1","settings":{"inverted":false,"restore_mode":"ALWAYS_ON",)"
                         R"("binding_input":"input_1","binding_mode":"follow"}})"));
  EXPECT_EQ("", this->settings.conflict());
  ASSERT_EQ(1u, this->settings.records().size());
  EXPECT_EQ(switch_::SWITCH_ALWAYS_ON, this->settings.records()[0]->restore_mode);
  EXPECT_EQ("follow", this->settings.records()[0]->binding_mode);
}

TEST_F(HeldRelay, ABadRequestIsABadRequestWhoeverHoldsTheRelay) {
  EXPECT_FALSE(this->post(R"({"source_name":"relay_1","settings":{"inverted":true,"restore_mode":"SOMETIMES"}})"));
  EXPECT_EQ("", this->settings.conflict());
}

// The answer is about the last request only: a refusal does not stick to the next one.
TEST_F(HeldRelay, AFreeRelayFlipsAndTheRefusalIsForgotten) {
  EXPECT_FALSE(this->post(R"({"source_name":"relay_1","settings":{"inverted":true}})"));
  ASSERT_TRUE(this->post(R"({"source_name":"relay_2","settings":{"inverted":true}})"));
  EXPECT_EQ("", this->settings.conflict());
  EXPECT_TRUE(e.relay2.is_inverted());

  this->holder.held.clear();
  ASSERT_TRUE(this->post(R"({"source_name":"relay_1","settings":{"inverted":true}})"));
  EXPECT_TRUE(e.relay1.is_inverted());
}

TEST_F(HeldRelay, TheMenuRowCannotFlipInvertedEither) {
  EXPECT_FALSE(this->settings.set_option(&e.relay1, Field::INVERTED, 1));
  EXPECT_FALSE(e.relay1.is_inverted());
  EXPECT_EQ(e.relay1.writes, 0);
  EXPECT_TRUE(this->settings.records().empty());

  EXPECT_TRUE(this->settings.set_option(&e.relay1, Field::INVERTED, 0)) << "the value it has is no flip";
  EXPECT_TRUE(this->settings.set_option(&e.relay1, Field::RESTORE_MODE, 1));
  EXPECT_EQ(switch_::SWITCH_ALWAYS_ON, e.relay1.restore_mode);
  EXPECT_FALSE(this->settings.set_option(&e.relay1, Field::RESTORE_MODE, 9)) << "out of range";

  EXPECT_TRUE(this->settings.set_option(&e.relay2, Field::INVERTED, 1));
  EXPECT_TRUE(e.relay2.is_inverted());
}

TEST(InvertedRefusal, NamesTheRelayAndItsHolder) {
  Entities &e = entities();
  EXPECT_EQ("", inverted_refusal(&e.relay1)) << "nothing registered";
  FakeHolder holder;
  holder.held[&e.relay1] = "Living room";
  switch_hold::set_holder(&holder);
  EXPECT_EQ(HELD_RELAY_1, inverted_refusal(&e.relay1));
  EXPECT_EQ("", inverted_refusal(&e.relay2));
  switch_hold::set_holder(nullptr);
}

}  // namespace esphome::entity_config::testing
