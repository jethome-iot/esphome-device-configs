#include "common.h"

namespace esphome::entity_config::testing {

using Field = SwitchSettingsJson::Field;

TEST(SwitchRecord, RoundTripsThroughJsonAndDefaultsTheRest) {
  SwitchSettingsRecord record;
  EXPECT_TRUE(load(R"({"source_name":"relay_1","restore_mode":"ALWAYS_ON","inverted":true,)"
                   R"("binding_input":"input_2","binding_mode":"follow"})",
                   record));
  EXPECT_EQ(record.restore_mode, switch_::SWITCH_ALWAYS_ON);
  EXPECT_TRUE(record.inverted);
  EXPECT_EQ(record.binding_input, "input_2");
  EXPECT_EQ(record.binding_mode, "follow");
  EXPECT_EQ(record.key(), fnv1_hash("relay_1"));
  EXPECT_EQ(dump(record), R"({"source_name":"relay_1","restore_mode":"ALWAYS_ON","inverted":true,)"
                          R"("binding_input":"input_2","binding_mode":"follow"})");

  // A field left out, or named wrongly, is marked so apply() takes the compiled value.
  SwitchSettingsRecord sparse;
  EXPECT_TRUE(load(R"({"source_name":"relay_2","restore_mode":"NO_SUCH_MODE"})", sparse));
  EXPECT_FALSE(sparse.has_restore_mode);
  EXPECT_FALSE(sparse.has_inverted);
  EXPECT_EQ(sparse.binding_input, "");
  EXPECT_EQ(sparse.binding_mode, "none");
  EXPECT_TRUE(record.has_restore_mode);
  EXPECT_TRUE(record.has_inverted);

  SwitchSettingsRecord nameless;
  EXPECT_FALSE(load(R"({"restore_mode":"ALWAYS_ON"})", nameless));
  EXPECT_FALSE(load(R"({"source_name":5})", nameless));
}

TEST(RestoreModeNames, EveryModeHasANameAndBack) {
  for (const auto &entry : RESTORE_MODE_NAMES) {
    switch_::SwitchRestoreMode mode = switch_::SWITCH_RESTORE_DISABLED;
    EXPECT_TRUE(parse_restore_mode(restore_mode_to_string(entry.mode), mode));
    EXPECT_EQ(mode, entry.mode);
  }
  switch_::SwitchRestoreMode mode = switch_::SWITCH_ALWAYS_ON;
  EXPECT_FALSE(parse_restore_mode(nullptr, mode));
  EXPECT_FALSE(parse_restore_mode("bogus", mode));
  EXPECT_EQ(mode, switch_::SWITCH_ALWAYS_ON);  // untouched
}

class SwitchSettings : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_entities();
    LogCapture::instance().warnings.clear();
  }
  void TearDown() override {
    manager().remove_binding(fnv1_hash("relay_1"));
    manager().remove_binding(fnv1_hash("relay_2"));
    reset_entities();
  }

  // A record as the file would have delivered it, before apply().
  SwitchSettingsRecord *stored(const char *json) {
    auto *record = new SwitchSettingsRecord();  // NOLINT(cppcoreguidelines-owning-memory)
    EXPECT_TRUE(load(json, *record));
    this->settings.records().push_back(record);
    return record;
  }
  SwitchSettingsRecord *record_of(const char *object_id) {
    for (auto *r : this->settings.records()) {
      if (r->source_name_ == object_id)
        return r;
    }
    return nullptr;
  }

  SwitchSettingsJson settings;
  Entities &e = entities();
};

TEST_F(SwitchSettings, AppliesTheStoredRecordsAtBootWithoutDrivingThePins) {
  stored(R"({"source_name":"relay_1","restore_mode":"ALWAYS_ON","inverted":true})");
  stored(R"({"source_name":"no_such_relay","restore_mode":"ALWAYS_ON"})");
  settings.apply();
  EXPECT_EQ(e.relay1.restore_mode, switch_::SWITCH_ALWAYS_ON);
  EXPECT_TRUE(e.relay1.is_inverted());
  EXPECT_EQ(e.relay1.writes, 0);  // the switch's own setup() drives the pin at boot
  EXPECT_EQ(e.relay2.restore_mode, switch_::SWITCH_RESTORE_DEFAULT_OFF);
  EXPECT_TRUE(LogCapture::instance().has("Switch not found for source_name 'no_such_relay'"));
  EXPECT_FALSE(settings.is_dirty());
}

TEST_F(SwitchSettings, ASparseRecordKeepsTheCompiledValuesAndTakesThemOver) {
  // A hand-written record that only binds: start mode and inversion stay as compiled.
  e.relay2.restore_mode = switch_::SWITCH_ALWAYS_ON;
  e.relay2.set_inverted(true);
  auto *record = stored(R"({"source_name":"relay_2","binding_input":"input_1","binding_mode":"toggle"})");
  auto *odd = stored(R"({"source_name":"relay_1","restore_mode":"SOMETIMES"})");
  settings.apply();
  EXPECT_EQ(e.relay2.restore_mode, switch_::SWITCH_ALWAYS_ON);
  EXPECT_TRUE(e.relay2.is_inverted());
  EXPECT_EQ(record->restore_mode, switch_::SWITCH_ALWAYS_ON);  // filled in, so the file says it too
  EXPECT_TRUE(record->inverted);
  EXPECT_TRUE(record->has_restore_mode);
  EXPECT_EQ(e.relay1.restore_mode, switch_::SWITCH_RESTORE_DEFAULT_OFF);
  EXPECT_EQ(odd->restore_mode, switch_::SWITCH_RESTORE_DEFAULT_OFF);
  EXPECT_TRUE(LogCapture::instance().has("Unknown restore_mode 'SOMETIMES'"));
  EXPECT_EQ(dump(*record), R"({"source_name":"relay_2","restore_mode":"ALWAYS_ON","inverted":true,)"
                           R"("binding_input":"input_1","binding_mode":"toggle"})");
}

TEST_F(SwitchSettings, DescribesEveryFieldAsAListOfOptions) {
  settings.apply();
  EXPECT_EQ(settings.option_count(Field::INVERTED), 2u);
  EXPECT_EQ(settings.option_count(Field::RESTORE_MODE), 3u);
  EXPECT_EQ(settings.option_count(Field::BINDING_INPUT), 3u);  // None + the two inputs
  EXPECT_EQ(settings.option_count(Field::BINDING_MODE), 3u);

  EXPECT_EQ(settings.option_label(&e.relay1, Field::INVERTED, 0), "No");
  EXPECT_EQ(settings.option_label(&e.relay1, Field::INVERTED, 1), "Yes");
  EXPECT_EQ(settings.option_label(&e.relay1, Field::RESTORE_MODE, 0), "Off");
  EXPECT_EQ(settings.option_label(&e.relay1, Field::RESTORE_MODE, 1), "On");
  EXPECT_EQ(settings.option_label(&e.relay1, Field::RESTORE_MODE, 2), "Last");
  EXPECT_EQ(settings.option_label(&e.relay1, Field::BINDING_INPUT, 0), "None");
  EXPECT_EQ(settings.option_label(&e.relay1, Field::BINDING_INPUT, 1), "Input 1");
  EXPECT_EQ(settings.option_label(&e.relay1, Field::BINDING_INPUT, 2), "Input 2");
  EXPECT_EQ(settings.option_label(&e.relay1, Field::BINDING_MODE, 0), "Disabled");
  EXPECT_EQ(settings.option_label(&e.relay1, Field::BINDING_MODE, 1), "Toggle");
  EXPECT_EQ(settings.option_label(&e.relay1, Field::BINDING_MODE, 2), "Follow");

  // Without a record the switch's own values show: the compiled restore mode is Last.
  EXPECT_EQ(settings.option_index(&e.relay1, Field::INVERTED), 0);
  EXPECT_EQ(settings.option_index(&e.relay1, Field::RESTORE_MODE), 2);
  EXPECT_EQ(settings.option_index(&e.relay1, Field::BINDING_INPUT), 0);
  EXPECT_EQ(settings.option_index(&e.relay1, Field::BINDING_MODE), 0);
  // -1 names the stored value as such, for the menu row of a value the list has no index for.
  EXPECT_EQ(settings.option_label(&e.relay1, Field::RESTORE_MODE, -1), "RESTORE_DEFAULT_OFF");
}

TEST_F(SwitchSettings, NamesAHandEditedValueTheListDoesNotOffer) {
  stored(R"({"source_name":"relay_2","restore_mode":"RESTORE_INVERTED_DEFAULT_ON",)"
         R"("binding_input":"gone_input","binding_mode":"sometimes"})");
  settings.apply();
  EXPECT_EQ(settings.option_index(&e.relay2, Field::RESTORE_MODE), -1);
  EXPECT_EQ(settings.option_label(&e.relay2, Field::RESTORE_MODE, -1), "RESTORE_INVERTED_DEFAULT_ON");
  EXPECT_EQ(settings.option_index(&e.relay2, Field::BINDING_INPUT), -1);
  EXPECT_EQ(settings.option_label(&e.relay2, Field::BINDING_INPUT, -1), "gone_input");
  EXPECT_EQ(settings.option_index(&e.relay2, Field::BINDING_MODE), -1);
  EXPECT_EQ(settings.option_label(&e.relay2, Field::BINDING_MODE, -1), "sometimes");
  EXPECT_TRUE(LogCapture::instance().has("Unknown binding mode 'sometimes'"));
}

TEST_F(SwitchSettings, SetOptionAppliesAtOnceAndRedrivesThePin) {
  settings.apply();
  settings.set_option(&e.relay1, Field::INVERTED, 1);
  EXPECT_TRUE(e.relay1.is_inverted());
  EXPECT_EQ(e.relay1.writes, 1);  // re-driven so the flipped output shows
  EXPECT_FALSE(e.relay1.state);   // the logical state is untouched
  EXPECT_TRUE(settings.is_dirty());
  ASSERT_NE(record_of("relay_1"), nullptr);
  EXPECT_TRUE(record_of("relay_1")->inverted);
  EXPECT_EQ(settings.option_index(&e.relay1, Field::INVERTED), 1);

  // Out of range: nothing happens.
  settings.set_option(&e.relay1, Field::RESTORE_MODE, 3);
  EXPECT_EQ(record_of("relay_1")->restore_mode, switch_::SWITCH_RESTORE_DEFAULT_OFF);
  EXPECT_EQ(e.relay1.writes, 1);

  settings.set_option(&e.relay1, Field::RESTORE_MODE, 1);
  EXPECT_EQ(e.relay1.restore_mode, switch_::SWITCH_ALWAYS_ON);
  EXPECT_EQ(record_of("relay_1")->restore_mode, switch_::SWITCH_ALWAYS_ON);
  EXPECT_EQ(e.relay1.writes, 1);             // only a flipped inversion touches the pin
  EXPECT_EQ(record_of("relay_2"), nullptr);  // only the edited switch gets a record
}

TEST_F(SwitchSettings, AFirstEditKeepsTheSwitchsOtherValues) {
  e.relay2.restore_mode = switch_::SWITCH_ALWAYS_ON;
  settings.apply();
  settings.set_option(&e.relay2, Field::INVERTED, 1);
  EXPECT_EQ(record_of("relay_2")->restore_mode, switch_::SWITCH_ALWAYS_ON);
  EXPECT_EQ(e.relay2.restore_mode, switch_::SWITCH_ALWAYS_ON);
}

TEST_F(SwitchSettings, ChoosingAPersistentModeStoresTheStateAtOnce) {
  e.relay1.restore_mode = switch_::SWITCH_ALWAYS_OFF;
  settings.apply();
  e.relay1.turn_on();  // ALWAYS_OFF: the switch saves nothing
  settings.set_option(&e.relay1, Field::RESTORE_MODE, 2);
  EXPECT_EQ(e.relay1.restore_mode, switch_::SWITCH_RESTORE_DEFAULT_OFF);
  bool saved = false;
  ASSERT_TRUE(e.relay1.make_entity_preference<bool>().load(&saved));
  EXPECT_TRUE(saved);
  // From here the switch keeps its own preference up to date.
  e.relay1.turn_off();
  ASSERT_TRUE(e.relay1.make_entity_preference<bool>().load(&saved));
  EXPECT_FALSE(saved);
}

TEST_F(SwitchSettings, BindingFieldsReachTheManager) {
  manager();
  settings.apply();
  settings.set_option(&e.relay1, Field::BINDING_INPUT, 1);  // Input 1
  e.in1.publish_state(true);
  e.in1.publish_state(false);
  EXPECT_EQ(e.relay1.writes, 0);                           // an input under mode Disabled is inactive
  settings.set_option(&e.relay1, Field::BINDING_MODE, 1);  // Toggle
  e.in1.publish_state(true);
  EXPECT_TRUE(e.relay1.state);
  e.in1.publish_state(false);
  settings.set_option(&e.relay1, Field::BINDING_MODE, 2);  // Follow: copies the input at once
  EXPECT_FALSE(e.relay1.state);
  e.in1.publish_state(true);
  EXPECT_TRUE(e.relay1.state);
  settings.set_option(&e.relay1, Field::BINDING_MODE, 0);
  e.in1.publish_state(false);
  EXPECT_TRUE(e.relay1.state);  // unbound
  // The record keeps the input for when the binding is enabled again.
  EXPECT_EQ(record_of("relay_1")->binding_input, "input_1");
  EXPECT_EQ(record_of("relay_1")->binding_mode, "none");
}

TEST_F(SwitchSettings, StoredBindingsAreAppliedAtBoot) {
  manager();
  stored(R"({"source_name":"relay_2","binding_input":"input_2","binding_mode":"toggle"})");
  settings.apply();
  e.in2.publish_state(true);
  EXPECT_TRUE(e.relay2.state);
}

}  // namespace esphome::entity_config::testing
