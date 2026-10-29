// A virtual input wherever an input is taken: a binding's source, a rule's trigger and condition,
// a label of entity_config's, Bind to's choices; and the boot order that makes the stored ones find it.
#include "common.h"

namespace esphome::virtual_inputs::testing {

static const char *const FILE_NAME = "virtual_inputs.json";
static const char *const SOMEONE_HOME = R"({"records":[{"slot":1,"source":"input_1","off_delay":600}]})";

static automations::AutomationConfig rule(const char *json) {
  JsonDocument doc;
  EXPECT_EQ(deserializeJson(doc, json), DeserializationError::Ok) << json;
  automations::AutomationConfig config;
  EXPECT_TRUE(config.deserialize(doc.as<JsonObject>())) << json;
  return config;
}

static JsonDocument body(const char *json) {
  JsonDocument doc;
  EXPECT_EQ(deserializeJson(doc, json), DeserializationError::Ok) << json;
  return doc;
}

TEST(BootOrder, ThePoolSetsUpBetweenTheKeepersLoadAndTheEntitySettingsApply) {
  config_json::ConfigJsonKeeper keeper;
  VirtualInputs inputs;
  config_json::global_config_json_keeper = &store().keeper;
  EXPECT_LT(inputs.get_setup_priority(), keeper.get_setup_priority());
  EXPECT_GT(inputs.get_setup_priority(), entity_config::SwitchSettingsJson::APPLY_PRIORITY);
  EXPECT_GT(inputs.get_setup_priority(), entity_config::BinarySensorSettingsJson::APPLY_PRIORITY);
  bindings::BindingsManager *latest = bindings::global_bindings_manager;
  bindings::BindingsManager manager;
  bindings::global_bindings_manager = latest;
  EXPECT_GT(inputs.get_setup_priority(), manager.get_setup_priority());
}

TEST_F(Pool, AStoredBindingFindsTheVirtualInputAtBootAndFollowsItsDelays) {
  write(FILE_NAME, SOMEONE_HOME);
  write("switch.json", R"({"version":1,"records":[)"
                       R"({"source_name":"relay_1","binding_input":"virtual_input_1","binding_mode":"follow"},)"
                       R"({"source_name":"relay_2","binding_input":"virtual_input_1","binding_mode":"toggle"}]})");
  bindings::BindingsManager &manager = new_manager();
  boot(true);
  manager.setup();
  EXPECT_FALSE(log().has_warning("not found for output"));
  // The boot drive: the slot took its source's level, so the follow relay is on.
  EXPECT_TRUE(e.relay1.state);
  EXPECT_FALSE(e.relay2.state);  // a boot level is no edge

  // The motion stops: the relay holds through the off delay, and a new pulse starts it again.
  e.in1.publish_state(false);
  pool().advance(599999);
  EXPECT_TRUE(e.relay1.state);
  e.in1.publish_state(true);
  e.in1.publish_state(false);
  pool().advance(599999);
  EXPECT_TRUE(e.relay1.state);
  pool().advance(1);
  EXPECT_FALSE(e.relay1.state);

  // The next rising edge of the virtual input toggles the other relay.
  e.in1.publish_state(true);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_TRUE(e.relay2.state);
}

TEST_F(Pool, ABindingAppliedBeforeThePoolSetsUpFindsNoInput) {
  // Why the pool's priority sits above the apply: out of order, the listener is never attached.
  write(FILE_NAME, SOMEONE_HOME);
  write("switch.json", R"({"version":1,"records":[)"
                       R"({"source_name":"relay_1","binding_input":"virtual_input_1","binding_mode":"follow"}]})");
  bindings::BindingsManager &manager = new_manager();
  store().keeper.setup();
  store().relays.apply();
  pool().setup();
  e.in1.publish_state(true);
  manager.setup();
  EXPECT_TRUE(log().has_warning("not found for output"));
  EXPECT_EQ(e.relay1.writes, 1);  // the boot drive, which looks the input up again
  e.in1.publish_state(false);
  pool().advance(600000);
  EXPECT_FALSE(pool().sensor(0)->state);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 1);
}

TEST_F(Pool, ABindingMadeAfterBootTakesANewVirtualInput) {
  bindings::BindingsManager &manager = new_manager();
  boot();
  manager.setup();
  ASSERT_TRUE(pool().save(1, follow_config("input_2", 0, 5)));
  // Bind to, from the panel: the new input is one of its choices, by its label.
  using Field = entity_config::SwitchSettingsJson::Field;
  JsonDocument labelled = body(R"({"source_name":"virtual_input_2","settings":{"label":"Staircase"}})");
  ASSERT_NE(store().inputs.update_record(labelled.as<JsonObject>()), nullptr);
  const size_t count = store().relays.option_count(Field::BINDING_INPUT);
  ASSERT_EQ(count, 5u);  // None, the three inputs, the virtual one
  EXPECT_EQ(store().relays.option_label(&e.relay1, Field::BINDING_INPUT, 4), "Staircase");
  ASSERT_TRUE(store().relays.set_option(&e.relay1, Field::BINDING_INPUT, 4));
  ASSERT_TRUE(store().relays.set_option(&e.relay1, Field::BINDING_MODE, 2));  // Follow
  EXPECT_FALSE(e.relay1.state);
  e.in2.publish_state(true);
  EXPECT_TRUE(e.relay1.state);
  e.in2.publish_state(false);
  pool().advance(4999);
  EXPECT_TRUE(e.relay1.state);
  pool().advance(1);
  EXPECT_FALSE(e.relay1.state);
  store().keeper.cancel_pending_save();
}

TEST_F(Pool, BindToShowsTheStoredInputForAChoiceTheListLostWhileEditing) {
  boot();
  ASSERT_TRUE(pool().save(1, follow_config("input_2", 0, 5)));
  using Field = entity_config::SwitchSettingsJson::Field;
  ASSERT_EQ(store().relays.option_count(Field::BINDING_INPUT), 5u);
  // The panel's row holds the last choice while the dashboard deletes it.
  ASSERT_TRUE(pool().remove(1));
  EXPECT_EQ(store().relays.option_label(&e.relay1, Field::BINDING_INPUT, 4), "None");
  EXPECT_FALSE(store().relays.set_option(&e.relay1, Field::BINDING_INPUT, 4));
  store().keeper.cancel_pending_save();
}

TEST_F(Pool, ADeleteDropsTheBindingsToItsSlotAndLeavesTheRelaysWhereTheyAre) {
  write(FILE_NAME, SOMEONE_HOME);
  write("switch.json", R"({"version":1,"records":[)"
                       R"({"source_name":"relay_1","binding_input":"virtual_input_1","binding_mode":"follow"},)"
                       R"({"source_name":"relay_2","binding_input":"input_2","binding_mode":"toggle"}]})");
  bindings::BindingsManager &manager = new_manager();
  boot(true);
  manager.setup();
  EXPECT_TRUE(e.relay1.state);
  ASSERT_TRUE(pool().remove(0));
  // Bind to: None, kept like any settings change.
  entity_config::SwitchSettingsRecord record;
  ASSERT_TRUE(store().relays.get_record(&e.relay1, record));
  EXPECT_EQ(record.binding_input, "");
  EXPECT_TRUE(store().keeper.is_save_pending());
  // The relay is left where it was, and the next virtual input in the slot drives it no more.
  EXPECT_TRUE(e.relay1.state);
  const int writes = e.relay1.writes;
  ASSERT_TRUE(pool().save(0, follow_config("input_2")));
  e.in2.publish_state(true);
  e.in2.publish_state(false);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, writes);
  EXPECT_TRUE(e.relay2.state);  // the binding to another input stays

  // The next boot finds no binding to the slot: nothing to warn about, nothing to drive.
  store().keeper.save_immediate();
  EXPECT_EQ(read("switch.json").find("virtual_input_1"), std::string::npos);
  bindings::BindingsManager &next = new_manager();
  log().clear();
  reboot(false, true);
  next.setup();
  EXPECT_FALSE(log().has_warning("not found for output"));
  ASSERT_TRUE(pool().sensor(0)->state);
  EXPECT_FALSE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 0);
}

TEST_F(Pool, ANewVirtualInputTakesTheLowestFreeSlotNothingNames) {
  // Slot 2 is named by a rule that found no input at boot, slot 4 by a hand-edited binding.
  write_rule("gated.json", R"({"id":1,"name":"Hall light when someone is home","triggers":[{"source":"input",)"
                           R"("type":"press","object_id":"input_2"}],"condition":{"type":"input",)"
                           R"("object_id":"virtual_input_2","state":"true"},"actions":[]})");
  write("switch.json", R"({"version":1,"records":[)"
                       R"({"source_name":"relay_2","binding_input":"virtual_input_4","binding_mode":"follow"}]})");
  new_manager();
  boot();
  TestEngine &engine = new_engine();
  engine.setup();
  EXPECT_EQ(pool().slot_for_new(), 0);
  // Slot 1 is named by a rule made while its input was in use.
  ASSERT_TRUE(pool().save(0, follow_config("input_1", 0, 600)));
  ASSERT_NE(engine.add_automation(rule(R"({"name":"Comfort while someone is home","triggers":[{"source":"input",)"
                                       R"("type":"state_change","object_id":"virtual_input_1"}],"actions":[]})")),
            0u);
  ASSERT_TRUE(pool().remove(0));
  EXPECT_EQ(pool().slot_for_new(), 2);
  ASSERT_TRUE(pool().save(2, follow_config("input_2")));
  // Every free slot is named: the lowest of them.
  EXPECT_EQ(pool().slot_for_new(), 0);
  store().keeper.cancel_pending_save();
}

TEST_F(Pool, ASlotIsUsedByTheRulesThatNameItBuiltOrNotAndTheRelaysBoundToIt) {
  write(FILE_NAME, R"({"records":[{"slot":1,"source":"input_1","off_delay":600},{"slot":2,"source":"input_2"}]})");
  write("switch.json", R"({"version":1,"records":[)"
                       R"({"source_name":"relay_2","binding_input":"virtual_input_1","binding_mode":"toggle"},)"
                       R"({"source_name":"relay_1","binding_input":"virtual_input_1","binding_mode":"follow",)"
                       R"("label":"Staircase"}]})");
  write_rule("comfort.json", R"({"id":1,"name":"Comfort while someone is home","triggers":[{"source":"input",)"
                             R"("type":"state_change","object_id":"virtual_input_1"}],"actions":[]})");
  // Not built: its trigger names an input this firmware lacks.
  write_rule("away.json", R"({"id":2,"name":"Away","triggers":[{"source":"input","type":"press",)"
                          R"("object_id":"no_such_input"}],"condition":{"type":"input",)"
                          R"("object_id":"virtual_input_1","state":"false"},"actions":[]})");
  new_manager();
  boot();
  // Without the rule engine, only the relays.
  EXPECT_TRUE(pool().users(0).rules.empty());
  EXPECT_EQ(pool().users(0).relays, (std::vector<std::string>{"Staircase", "Relay 2"}));
  TestEngine &engine = new_engine();
  engine.setup();
  const SlotUsers users = pool().users(0);
  EXPECT_EQ(users.rules, (std::vector<std::string>{"Comfort while someone is home", "Away"}));
  EXPECT_EQ(users.relays, (std::vector<std::string>{"Staircase", "Relay 2"}));
  for (size_t slot : {1, 2, 9}) {
    EXPECT_TRUE(pool().users(slot).rules.empty()) << slot;
    EXPECT_TRUE(pool().users(slot).relays.empty()) << slot;
  }
}

TEST_F(Pool, ARuleTriggersOnAVirtualInputAndReadsItAsACondition) {
  boot();
  TestEngine &engine = new_engine();
  engine.setup();
  ASSERT_TRUE(pool().save(0, follow_config("input_1", 0, 600)));
  std::string error;
  const uint32_t follow = engine.add_automation(
      rule(R"({"name":"Comfort while someone is home","triggers":[{"source":"input","type":"state_change",)"
           R"("object_id":"virtual_input_1"}],"actions":[{"source":"switch","type":"follow","object_id":"relay_2",)"
           R"("invert":false}]})"),
      &error);
  ASSERT_NE(follow, 0u) << error;
  const uint32_t gated = engine.add_automation(
      rule(R"({"name":"Hall light when someone is home","triggers":[{"source":"input","type":"press",)"
           R"("object_id":"input_2"}],"condition":{"type":"input","object_id":"virtual_input_1","state":"true"},)"
           R"("actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})"),
      &error);
  ASSERT_NE(gated, 0u) << error;

  e.in2.publish_state(true);
  EXPECT_FALSE(e.relay1.state);  // nobody home
  e.in2.publish_state(false);

  e.in1.publish_state(true);
  EXPECT_TRUE(e.relay2.state);
  e.in1.publish_state(false);
  pool().advance(300000);
  EXPECT_TRUE(e.relay2.state);  // a pause in the motion is no change of the virtual input
  EXPECT_EQ(e.relay2.writes, 1);
  e.in2.publish_state(true);
  EXPECT_TRUE(e.relay1.state);  // somebody is
  pool().advance(300000);
  EXPECT_FALSE(e.relay2.state);
  EXPECT_EQ(e.relay2.writes, 2);
}

TEST_F(Pool, ARuleNamingAFreeSlotIsRefusedAsForAnyMissingInput) {
  boot();
  TestEngine &engine = new_engine();
  engine.setup();
  std::string error;
  EXPECT_EQ(engine.add_automation(rule(R"({"name":"Nobody","triggers":[{"source":"input","type":"press",)"
                                       R"("object_id":"virtual_input_3"}],"actions":[]})"),
                                  &error),
            0u);
  EXPECT_NE(error.find("input not found"), std::string::npos) << error;
}

TEST_F(Pool, ALabelIsTheEntitySettingsRecordsAndAnInversionIsRefused) {
  boot(true);
  ASSERT_TRUE(pool().save(0, follow_config("input_1")));
  binary_sensor::BinarySensor *input = pool().sensor(0);
  JsonDocument labelled = body(R"({"source_name":"virtual_input_1","settings":{"label":"Someone home"}})");
  ASSERT_NE(store().inputs.update_record(labelled.as<JsonObject>()), nullptr);
  EXPECT_EQ(store().inputs.display_name(input), "Someone home");
  EXPECT_EQ(input->get_name().str(), "Virtual input 1");  // the name and the object id stay

  JsonDocument inverted = body(R"({"source_name":"virtual_input_1","settings":{"inverted":true}})");
  EXPECT_EQ(store().inputs.update_record(inverted.as<JsonObject>()), nullptr);
  JsonDocument not_inverted = body(R"({"source_name":"virtual_input_1","settings":{"inverted":false}})");
  EXPECT_NE(store().inputs.update_record(not_inverted.as<JsonObject>()), nullptr);
  store().inputs.set_inverted(input, true);  // the menu has no such row; the call changes nothing either
  EXPECT_FALSE(store().inputs.is_inverted(input));
  store().inputs.apply();
  EXPECT_TRUE(input->state);
  e.in1.publish_state(false);
  EXPECT_FALSE(input->state);
  store().keeper.cancel_pending_save();
}

TEST_F(Pool, AStoredInversionOnAVirtualInputIsIgnoredAtBoot) {
  write(FILE_NAME, SOMEONE_HOME);
  write("binary_sensor.json", R"({"version":1,"records":[)"
                              R"({"source_name":"virtual_input_1","inverted":true,"label":"Someone home"}]})");
  boot(true);
  EXPECT_TRUE(log().has_warning("'virtual_input_1' takes no inversion, ignoring it"));
  binary_sensor::BinarySensor *input = pool().sensor(0);
  EXPECT_TRUE(input->state);
  EXPECT_FALSE(store().inputs.is_inverted(input));
  EXPECT_EQ(store().inputs.display_name(input), "Someone home");
  // What a read of the record says from then on.
  entity_config::BinarySensorSettingsRecord record;
  ASSERT_TRUE(store().inputs.get_record(input, record));
  EXPECT_FALSE(record.inverted);
}

TEST_F(Pool, ADeleteDropsTheLabelSoTheNextInputInTheSlotStartsWithout) {
  write(FILE_NAME, SOMEONE_HOME);
  write("binary_sensor.json", R"({"version":1,"records":[{"source_name":"virtual_input_1","label":"Someone home"},)"
                              R"({"source_name":"input_3","label":"Hall motion"}]})");
  boot(true);
  ASSERT_EQ(store().inputs.get_label("virtual_input_1"), "Someone home");
  ASSERT_TRUE(pool().remove(0));
  EXPECT_EQ(store().inputs.get_label("virtual_input_1"), "");
  EXPECT_EQ(store().inputs.get_label("input_3"), "Hall motion");
  EXPECT_TRUE(store().keeper.is_save_pending());
  store().keeper.save_immediate();
  EXPECT_EQ(read("binary_sensor.json").find("virtual_input_1"), std::string::npos);
  ASSERT_TRUE(pool().save(0, follow_config("input_2")));
  EXPECT_EQ(store().inputs.display_name(pool().sensor(0)), "Virtual input 1");
}

TEST_F(Pool, AVirtualInputFollowsItsSourceAsReportedInversionIncluded) {
  write(FILE_NAME, R"({"records":[{"slot":1,"source":"input_3","off_delay":600}]})");
  write("binary_sensor.json", R"({"version":1,"records":[{"source_name":"input_3","inverted":true}]})");
  boot();
  e.in3.publish_state(false);
  // The contact is open; inverted, the input reports on, and so does the virtual input.
  EXPECT_TRUE(e.in3.state);
  EXPECT_TRUE(pool().sensor(0)->state);
  store().inputs.set_inverted(&e.in3, false);
  EXPECT_FALSE(e.in3.state);
  pool().advance(600000);
  EXPECT_FALSE(pool().sensor(0)->state);
  store().keeper.cancel_pending_save();
}

}  // namespace esphome::virtual_inputs::testing
