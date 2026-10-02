#include "common.h"
#include "esphome/components/automations/automation_storage.h"
#include "esphome/components/bindings/bindings.h"

namespace esphome::mqtt_subscriptions::testing {

// A Number slot is a "Temperature" to the rule engine and an On/Off slot an "Input"; an On/Off
// slot can be a relay's bound input. Each runs here in boot order: the slots at 803, the
// binding the switch settings set at 801, bindings at 600, the engine at 599.
class SlotsDriveTest : public SlotsTest {
 protected:
  void SetUp() override {
    SlotsTest::SetUp();
    Entities &e = entities();
    e.relay1.publish_state(false);
    e.relay2.publish_state(false);
  }

  void rule(const char *file, const char *json) {
    const std::string folder = this->storage.get_base_path() + "/automations";
    mkdir(folder.c_str(), 0755);
    write_text(folder + "/" + file, json);
  }

  // Never freed: the slots' entities keep a callback into each.
  automations::AutomationStorage &engine() {
    auto *engine = new automations::AutomationStorage();
    engine->set_storage(&this->storage);
    engine->set_folder_path("automations");
    engine->setup();
    return *engine;
  }
};

TEST_F(SlotsDriveTest, ANumberSlotIsATemperatureTrigger) {
  this->rule("warm.json",
             R"({"id":1,"name":"Warm","enabled":true,"mode":"single",)"
             R"("triggers":[{"source":"temperature","type":"above","object_id":"outdoor","threshold":25}],)"
             R"("actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})");
  this->plant({slot_of("Outdoor", "z2m/outdoor")});
  this->boot();
  this->online();
  automations::AutomationStorage &engine = this->engine();
  ASSERT_EQ(engine.configs().size(), 1u);
  this->deliver("z2m/outdoor", "21");
  EXPECT_FALSE(entities().relay1.state);
  this->deliver("z2m/outdoor", "26.5");
  EXPECT_TRUE(entities().relay1.state);
}

TEST_F(SlotsDriveTest, AnOnOffSlotIsAnInputTrigger) {
  this->rule("door.json", R"({"id":2,"name":"Door","enabled":true,"mode":"single",)"
                          R"("triggers":[{"source":"input","type":"press","object_id":"door"}],)"
                          R"("actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})");
  this->plant({slot_of("Door", "z2m/door", SlotKind::BINARY_SENSOR)});
  this->boot();
  this->online();
  this->engine();
  // The retained state on connect is a level, not a press.
  this->deliver("z2m/door", "ON");
  EXPECT_FALSE(entities().relay1.state);
  this->deliver("z2m/door", "OFF");
  this->deliver("z2m/door", "ON");
  EXPECT_TRUE(entities().relay1.state);
}

TEST_F(SlotsDriveTest, AnOnOffSlotIsABoundInput) {
  this->plant({slot_of("Door", "z2m/door", SlotKind::BINARY_SENSOR)});
  this->boot();
  this->online();
  static auto *bindings = new bindings::BindingsManager();
  bindings->set_binding(fnv1_hash("relay_2"), fnv1_hash("door"), bindings::BindingMode::FOLLOW);
  bindings->setup();
  // Follow takes the first value too: it is the state the relay should be in.
  this->deliver("z2m/door", "ON");
  EXPECT_TRUE(entities().relay2.state);
  this->deliver("z2m/door", "OFF");
  EXPECT_FALSE(entities().relay2.state);
  bindings->remove_binding(fnv1_hash("relay_2"));
}

}  // namespace esphome::mqtt_subscriptions::testing
