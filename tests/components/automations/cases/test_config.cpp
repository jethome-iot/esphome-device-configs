#include "common.h"

namespace esphome::automations::testing {

// The scheduler reads UINT32_MAX as "never run", so a delay stops one below it.
static constexpr uint32_t MAX_DELAY_MS = 4294967294u;

// --- Triggers ---

TEST(TriggerConfig, InputTriggerHashesTheObjectId) {
  TriggerConfig t;
  ASSERT_TRUE(load(R"({"source":"input","type":"press","object_id":"in_1"})", t));
  EXPECT_EQ(t.source, SourceTrigger::INPUT);
  EXPECT_EQ(t.params.input.type, TypesInputTrigger::PRESS);
  EXPECT_EQ(t.params.input.input_id, fnv1_hash("in_1"));
}

TEST(TriggerConfig, SerializesTheObjectIdOfARegisteredEntity) {
  entities();
  TriggerConfig t;
  ASSERT_TRUE(load(R"({"source":"switch","type":"turn_on","object_id":"relay_1"})", t));
  EXPECT_EQ(dump(t), R"({"source":"switch","type":"turn_on","object_id":"relay_1"})");
}

TEST(TriggerConfig, SerializesAnUnknownEntityAsEmpty) {
  entities();
  TriggerConfig t;
  ASSERT_TRUE(load(R"({"source":"input","type":"press","object_id":"no_such_input"})", t));
  EXPECT_EQ(dump(t), R"({"source":"input","type":"press","object_id":""})");
}

TEST(TriggerConfig, TemperatureCarriesThresholds) {
  TriggerConfig below;
  ASSERT_TRUE(load(R"({"source":"temperature","type":"below","object_id":"temp","threshold":18.5})", below));
  EXPECT_EQ(below.params.temperature.type, TypesTemperatureTrigger::BELOW);
  EXPECT_FLOAT_EQ(below.params.temperature.threshold, 18.5f);

  TriggerConfig range;
  ASSERT_TRUE(load(
      R"({"source":"temperature","type":"range","object_id":"temp","min_threshold":10,"max_threshold":20})", range));
  EXPECT_EQ(range.params.temperature.type, TypesTemperatureTrigger::RANGE);
  EXPECT_FLOAT_EQ(range.params.temperature.min_threshold, 10.0f);
  EXPECT_FLOAT_EQ(range.params.temperature.max_threshold, 20.0f);
}

TEST(TriggerConfig, TemperatureNeedsItsThresholds) {
  TriggerConfig t;
  EXPECT_FALSE(load(R"({"source":"temperature","type":"below","object_id":"temp"})", t));
  EXPECT_FALSE(load(R"({"source":"temperature","type":"above","object_id":"temp","treshold":25})", t));
  EXPECT_FALSE(load(R"({"source":"temperature","type":"range","object_id":"temp","min_threshold":10})", t));
  EXPECT_FALSE(load(R"({"source":"temperature","type":"above","object_id":"temp","threshold":"hot"})", t));
}

TEST(TriggerConfig, StartupNeedsNothingElse) {
  TriggerConfig t;
  ASSERT_TRUE(load(R"({"source":"startup"})", t));
  EXPECT_EQ(t.source, SourceTrigger::STARTUP);
  EXPECT_EQ(dump(t), R"({"source":"startup"})");
}

TEST(TriggerConfig, RefusesWordsItDoesNotKnow) {
  TriggerConfig t;
  EXPECT_FALSE(load(R"({"type":"press","object_id":"in_1"})", t));
  EXPECT_FALSE(load(R"({"source":"inupt","type":"press","object_id":"in_1"})", t));
  EXPECT_FALSE(load(R"({"source":"input","type":"pres","object_id":"in_1"})", t));
  EXPECT_FALSE(load(R"({"source":"input","type":"none","object_id":"in_1"})", t));
  EXPECT_FALSE(load(R"({"source":"temperature","type":"between","object_id":"temp"})", t));
}

// --- Conditions ---

TEST(ConditionConfig, InputStateDefaultsToTrue) {
  ConditionConfig c;
  ASSERT_TRUE(load(R"({"type":"input","object_id":"in_2"})", c));
  EXPECT_EQ(c.type, ConditionType::INPUT);
  EXPECT_EQ(c.sensor_id, fnv1_hash("in_2"));
  EXPECT_EQ(c.state, InputConditionState::TRUE);

  ASSERT_TRUE(load(R"({"type":"input","object_id":"in_2","state":"false"})", c));
  EXPECT_EQ(c.state, InputConditionState::FALSE);
}

TEST(ConditionConfig, GroupsNest) {
  ConditionConfig c;
  ASSERT_TRUE(load(R"({"type":"and","conditions":[
      {"type":"input","object_id":"in_2"},
      {"type":"or","conditions":[
        {"type":"temperature","object_id":"temp","temperature_type":"above","threshold":25},
        {"type":"temperature","object_id":"temp","temperature_type":"range","min_threshold":1,"max_threshold":2}]}]})",
                   c));
  ASSERT_EQ(c.sub_conditions.size(), 2u);
  EXPECT_EQ(c.sub_conditions[1].type, ConditionType::OR);
  ASSERT_EQ(c.sub_conditions[1].sub_conditions.size(), 2u);
  EXPECT_FLOAT_EQ(c.sub_conditions[1].sub_conditions[0].threshold, 25.0f);
  EXPECT_FLOAT_EQ(c.sub_conditions[1].sub_conditions[1].max_threshold, 2.0f);
}

TEST(ConditionConfig, RefusesAnEmptyGroup) {
  ConditionConfig c;
  EXPECT_FALSE(load(R"({"type":"and"})", c));
  EXPECT_FALSE(load(R"({"type":"or","conditions":[]})", c));
}

TEST(ConditionConfig, RefusesABrokenMember) {
  ConditionConfig c;
  EXPECT_FALSE(load(R"({"type":"xor","conditions":[{"type":"input","object_id":"in_2"},{"type":"tempratur"}]})", c));
}

TEST(ConditionConfig, TemperatureNeedsAKnownTypeAndItsThresholds) {
  ConditionConfig c;
  EXPECT_FALSE(load(R"({"type":"temperature","object_id":"temp"})", c));
  EXPECT_FALSE(load(R"({"type":"temperature","object_id":"temp","temperature_type":"none"})", c));
  EXPECT_FALSE(load(R"({"type":"temperature","object_id":"temp","temperature_type":"below"})", c));
  EXPECT_FALSE(load(R"({"type":"temperature","object_id":"temp","temperature_type":"range","max_threshold":20})", c));
}

// --- Actions ---

TEST(ActionConfig, DelayReadsEitherUnitAndWritesMilliseconds) {
  ActionConfig a;
  ASSERT_TRUE(load(R"({"source":"delay","delay_ms":900})", a));
  EXPECT_EQ(a.params.delay.delay_ms, 900u);
  EXPECT_EQ(dump(a), R"({"source":"delay","delay_ms":900})");

  ASSERT_TRUE(load(R"({"source":"delay","delay_s":5})", a));
  EXPECT_EQ(a.params.delay.delay_ms, 5000u);
  EXPECT_EQ(dump(a), R"({"source":"delay","delay_ms":5000})");
}

TEST(ActionConfig, DelayNeedsOneOfItsUnits) {
  ActionConfig a;
  EXPECT_FALSE(load(R"({"source":"delay"})", a));
  EXPECT_FALSE(load(R"({"source":"delay","delay_mz":500})", a));
}

TEST(ActionConfig, DelayClampsToWhatTheSchedulerTakes) {
  ActionConfig a;
  ASSERT_TRUE(load(R"({"source":"delay","delay_s":5000000})", a));
  EXPECT_EQ(a.params.delay.delay_ms, MAX_DELAY_MS);
  ASSERT_TRUE(load(R"({"source":"delay","delay_ms":4294967295})", a));
  EXPECT_EQ(a.params.delay.delay_ms, MAX_DELAY_MS);
  ASSERT_TRUE(load(R"({"source":"delay","delay_ms":-5})", a));
  EXPECT_EQ(a.params.delay.delay_ms, 0u);
}

TEST(ActionConfig, InvertIsWrittenOnlyForFollow) {
  entities();
  ActionConfig a;
  ASSERT_TRUE(load(R"({"source":"switch","type":"follow","object_id":"relay_2","invert":true})", a));
  EXPECT_TRUE(a.params.switch_action.invert);
  EXPECT_EQ(dump(a), R"({"source":"switch","type":"follow","object_id":"relay_2","invert":true})");

  ASSERT_TRUE(load(R"({"source":"switch","type":"toggle","object_id":"relay_2","invert":true})", a));
  EXPECT_EQ(dump(a), R"({"source":"switch","type":"toggle","object_id":"relay_2"})");
}

TEST(ActionConfig, RefusesWordsItDoesNotKnow) {
  ActionConfig a;
  EXPECT_FALSE(load(R"({"type":"toggle","object_id":"relay_1"})", a));
  EXPECT_FALSE(load(R"({"source":"relay","type":"toggle","object_id":"relay_1"})", a));
  EXPECT_FALSE(load(R"({"source":"switch","type":"tugle","object_id":"relay_1"})", a));
}

// --- Whole rules ---

static const char *const RULE = R"({"id":7,"name":"Cond","enabled":true,"mode":"restart",
  "triggers":[{"source":"input","type":"press","object_id":"in_1"}],
  "condition":{"type":"input","object_id":"in_2","state":"true"},
  "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}],
  "else_actions":[{"source":"switch","type":"turn_on","object_id":"relay_2"}]})";

TEST(AutomationConfig, RoundTripsARule) {
  entities();
  AutomationConfig c;
  ASSERT_TRUE(load(RULE, c));
  EXPECT_EQ(c.id, 7u);
  EXPECT_EQ(c.name, "Cond");
  EXPECT_TRUE(c.enabled);
  EXPECT_EQ(c.mode, AutomationMode::RESTART);
  ASSERT_EQ(c.triggers.size(), 1u);
  EXPECT_TRUE(c.condition.is_valid());
  ASSERT_EQ(c.actions.size(), 1u);
  ASSERT_EQ(c.else_actions.size(), 1u);

  AutomationConfig again;
  ASSERT_TRUE(load(dump(c).c_str(), again));
  EXPECT_EQ(dump(again), dump(c));
}

TEST(AutomationConfig, AbsentFieldsTakeTheirDefaults) {
  AutomationConfig c;
  ASSERT_TRUE(load(R"({"name":"Bare","triggers":[{"source":"startup"}]})", c));
  EXPECT_EQ(c.id, 0u);
  EXPECT_TRUE(c.enabled);
  EXPECT_EQ(c.mode, AutomationMode::SINGLE);
  EXPECT_FALSE(c.condition.is_valid());
  EXPECT_TRUE(c.actions.empty());
  EXPECT_TRUE(c.else_actions.empty());
}

TEST(AutomationConfig, ElseActionsAreWrittenOnlyWhenPresent) {
  AutomationConfig c;
  ASSERT_TRUE(load(R"({"name":"Bare","triggers":[{"source":"startup"}]})", c));
  EXPECT_EQ(dump(c),
            R"({"id":0,"name":"Bare","enabled":true,"mode":"single","triggers":[{"source":"startup"}],"actions":[]})");
}

TEST(AutomationConfig, RefusesWhatItCannotKeep) {
  AutomationConfig c;
  EXPECT_FALSE(load(R"({"triggers":[{"source":"startup"}]})", c));
  EXPECT_FALSE(load(R"({"name":"X","mode":"twice","triggers":[{"source":"startup"}]})", c));
  EXPECT_FALSE(load(R"({"name":"X","triggers":[{"source":"startup"},{"source":"inupt"}]})", c));
  EXPECT_FALSE(load(R"({"name":"X","triggers":[{"source":"startup"}],"condition":{"type":"and"}})", c));
  EXPECT_FALSE(load(R"({"name":"X","triggers":[{"source":"startup"}],"actions":[{"source":"relay"}]})", c));
}

}  // namespace esphome::automations::testing
