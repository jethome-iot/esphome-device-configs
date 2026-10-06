#include "common.h"
#include <map>
#include "esphome/components/switch_hold/switch_hold.h"

namespace esphome::automations::testing {

// Stands in for climate_hub: holds the switches the case puts in the table, by name.
class FakeHolder : public switch_hold::SwitchHolder {
 public:
  std::map<const switch_::Switch *, std::string> held;
  std::string holder_of(const switch_::Switch *sw) const override {
    auto it = this->held.find(sw);
    return it == this->held.end() ? std::string() : it->second;
  }
};

class Hold : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_entities();
    switch_hold::set_holder(&this->holder);
    this->holder.held[&this->e.relay1] = "Living room";
  }
  void TearDown() override { switch_hold::set_holder(nullptr); }
  FakeEngine engine;
  FakeHolder holder;
  Entities &e = entities();
};

// Every switch action leaves a held relay where the thermostat put it.
TEST_F(Hold, NoSwitchActionMovesAHeldRelay) {
  for (const char *type : {"turn_on", "turn_off", "toggle", "follow"}) {
    const std::string json = std::string(R"({"name":"Rule","triggers":[{"source":"input","type":"state_change",)"
                                         R"("object_id":"in_1"}],"actions":[{"source":"switch","type":")") +
                             type + R"(","object_id":"relay_1"}]})";
    auto rule = build_rule(engine, json.c_str());
    ASSERT_NE(rule, nullptr) << type;
    rule->on_binary_sensor(&e.in1, true);
    rule->on_binary_sensor(&e.in1, false);
    EXPECT_EQ(e.relay1.writes, 0) << type;
    EXPECT_FALSE(rule->is_running()) << type;
  }
}

// The step on the held relay is skipped; the run goes on, across a delay too.
TEST_F(Hold, TheRestOfTheRunStillRuns) {
  auto rule = build_rule(engine, R"({"name":"Both","triggers":[{"source":"startup"}],
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},
                 {"source":"switch","type":"turn_on","object_id":"relay_2"},
                 {"source":"delay","delay_ms":1000},
                 {"source":"switch","type":"toggle","object_id":"relay_1"},
                 {"source":"switch","type":"turn_off","object_id":"relay_2"}]})");
  ASSERT_NE(rule, nullptr);
  rule->on_startup();
  EXPECT_EQ(e.relay1.writes, 0);
  EXPECT_TRUE(e.relay2.state);

  this->holder.held.clear();  // the thermostat stopped while the rule waited
  ASSERT_TRUE(engine.fire_next());
  EXPECT_TRUE(e.relay1.state);
  EXPECT_FALSE(e.relay2.state);
  EXPECT_FALSE(rule->is_running());
}

}  // namespace esphome::automations::testing
