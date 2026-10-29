#include "common.h"

namespace esphome::automations::testing {

class Runtime : public ::testing::Test {
 protected:
  void SetUp() override { reset_entities(); }
  FakeEngine engine;
  Entities &e = entities();
};

TEST_F(Runtime, BuildRefusesWhatItCannotResolve) {
  EXPECT_EQ(build_rule(engine, R"({"name":"No triggers","triggers":[]})"), nullptr);
  EXPECT_EQ(
      build_rule(engine, R"({"name":"Orphan","triggers":[{"source":"input","type":"press","object_id":"no_such"}]})"),
      nullptr);
  EXPECT_EQ(build_rule(engine, R"({"name":"Orphan action","triggers":[{"source":"startup"}],
      "actions":[{"source":"switch","type":"toggle","object_id":"no_such"}]})"),
            nullptr);
  EXPECT_EQ(build_rule(engine, R"({"name":"Orphan condition","triggers":[{"source":"startup"}],
      "condition":{"type":"input","object_id":"no_such"}})"),
            nullptr);
  EXPECT_EQ(build_rule(engine, R"({"name":"No clock","triggers":[{"source":"cron","cron":"* * * * * *"}]})"), nullptr);
}

// A config switch is the device's own setting: a rule naming it, as its action or its trigger,
// is refused as one naming a switch that is not there, and the same rule on a relay builds.
TEST_F(Runtime, BuildRefusesAConfigSwitchAsAMissingOne) {
  static const char *const UNLOCK =
      R"({"name":"Unlock","triggers":[{"source":"startup"}],"actions":[{"source":"switch","type":"turn_on","object_id":"%s"}]})";
  static const char *const WATCH =
      R"({"name":"Watch","triggers":[{"source":"switch","type":"turn_on","object_id":"%s"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})";
  auto naming = [](std::string json, const char *object_id) {
    json.replace(json.find("%s"), 2, object_id);
    AutomationConfig config;
    EXPECT_TRUE(load(json.c_str(), config));
    return config;
  };
  for (const char *object_id : {"allow_esphome_updates", "no_such"}) {
    std::string error;
    EXPECT_EQ(RuntimeAutomation::build(&engine, naming(UNLOCK, object_id), &error), nullptr) << object_id;
    EXPECT_EQ(error, "Action 1: switch not found") << object_id;
    EXPECT_EQ(RuntimeAutomation::build(&engine, naming(WATCH, object_id), &error), nullptr) << object_id;
    EXPECT_EQ(error, "Trigger 1: switch not found") << object_id;
  }

  auto unlock = RuntimeAutomation::build(&engine, naming(UNLOCK, "relay_2"));
  ASSERT_NE(unlock, nullptr);
  unlock->on_startup();
  EXPECT_TRUE(e.relay2.state);
  auto watch = RuntimeAutomation::build(&engine, naming(WATCH, "relay_2"));
  ASSERT_NE(watch, nullptr);
  watch->on_switch(&e.relay2, true);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.ota_allowed.writes, 0);
}

TEST_F(Runtime, PressTurnsTheRelayOn) {
  auto rule = build_rule(engine, R"({"name":"Press","triggers":[{"source":"input","type":"press","object_id":"in_1"}],
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, false);
  EXPECT_EQ(e.relay1.writes, 0);
  rule->on_binary_sensor(&e.in2, true);
  EXPECT_EQ(e.relay1.writes, 0);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_FALSE(rule->is_running());
}

TEST_F(Runtime, ReleaseAndStateChangeSeeTheOtherEdge) {
  auto release =
      build_rule(engine, R"({"name":"Release","triggers":[{"source":"input","type":"release","object_id":"in_1"}],
      "actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})");
  auto change =
      build_rule(engine, R"({"name":"Change","triggers":[{"source":"input","type":"state_change","object_id":"in_1"}],
      "actions":[{"source":"switch","type":"toggle","object_id":"relay_2"}]})");
  ASSERT_NE(release, nullptr);
  ASSERT_NE(change, nullptr);
  for (bool state : {true, false}) {
    release->on_binary_sensor(&e.in1, state);
    change->on_binary_sensor(&e.in1, state);
  }
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_EQ(e.relay2.writes, 2);
}

TEST_F(Runtime, FollowCopiesTheTriggerState) {
  auto rule = build_rule(
      engine, R"({"name":"Follow","triggers":[{"source":"switch","type":"state_change","object_id":"relay_1"}],
      "actions":[{"source":"switch","type":"follow","object_id":"relay_2","invert":true}]})");
  ASSERT_NE(rule, nullptr);
  rule->on_switch(&e.relay1, true);
  EXPECT_FALSE(e.relay2.state);
  rule->on_switch(&e.relay1, false);
  EXPECT_TRUE(e.relay2.state);
  EXPECT_EQ(e.relay2.writes, 2);
}

TEST_F(Runtime, FollowNeedsAStateToCopy) {
  auto rule = build_rule(engine, R"({"name":"Follow","triggers":[{"source":"startup"}],
      "actions":[{"source":"switch","type":"follow","object_id":"relay_2"}]})");
  ASSERT_NE(rule, nullptr);
  rule->on_startup();
  EXPECT_EQ(e.relay2.writes, 0);
}

TEST_F(Runtime, TemperatureFiresOncePerCrossing) {
  auto rule = build_rule(
      engine, R"({"name":"Hot","triggers":[{"source":"temperature","type":"above","object_id":"temp","threshold":25}],
      "actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  rule->on_sensor(&e.temp, 20);
  rule->on_sensor(&e.temp, 26);
  rule->on_sensor(&e.temp, 30);
  EXPECT_EQ(e.relay1.writes, 1);
  rule->on_sensor(&e.temp, 25);  // back on the threshold re-arms
  rule->on_sensor(&e.temp, 26);
  EXPECT_EQ(e.relay1.writes, 2);
  rule->on_sensor(&e.temp, NAN);
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(Runtime, RangeFiresOnEntry) {
  auto rule = build_rule(
      engine,
      R"({"name":"Mild","triggers":[{"source":"temperature","type":"range","object_id":"temp","min_threshold":10,"max_threshold":20}],
      "actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  rule->on_sensor(&e.temp, 15);  // already inside at the first reading counts as entering
  rule->on_sensor(&e.temp, 18);
  EXPECT_EQ(e.relay1.writes, 1);
  rule->on_sensor(&e.temp, 25);
  rule->on_sensor(&e.temp, 20);
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(Runtime, ConditionPicksTheBranch) {
  auto rule = build_rule(engine, R"({"name":"Cond","triggers":[{"source":"input","type":"press","object_id":"in_1"}],
      "condition":{"type":"and","conditions":[
        {"type":"input","object_id":"in_2"},
        {"type":"temperature","object_id":"temp","temperature_type":"below","threshold":20}]},
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}],
      "else_actions":[{"source":"switch","type":"turn_on","object_id":"relay_2"}]})");
  ASSERT_NE(rule, nullptr);
  // No temperature yet: the condition is false, not unknown.
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 0);
  EXPECT_EQ(e.relay2.writes, 1);
  e.in2.publish_state(true);
  e.temp.state = 15;
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_EQ(e.relay2.writes, 1);
}

TEST_F(Runtime, XorWantsExactlyOne) {
  CompiledCondition c;
  ConditionConfig config;
  ASSERT_TRUE(
      load(R"({"type":"xor","conditions":[{"type":"input","object_id":"in_1"},{"type":"input","object_id":"in_2"}]})",
           config));
  ASSERT_TRUE(compile_condition(config, c));
  EXPECT_FALSE(c.check());
  e.in1.publish_state(true);
  EXPECT_TRUE(c.check());
  e.in2.publish_state(true);
  EXPECT_FALSE(c.check());
}

static const char *const DELAYED =
    R"({"name":"Delayed","mode":"%s","triggers":[{"source":"input","type":"press","object_id":"in_1"}],
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},{"source":"delay","delay_ms":500},
                 {"source":"switch","type":"toggle","object_id":"relay_2"}]})";

static std::string delayed(const char *mode) {
  char buf[512];
  snprintf(buf, sizeof(buf), DELAYED, mode);
  return buf;
}

TEST_F(Runtime, DelayWaitsForTheScheduler) {
  auto rule = build_rule(engine, delayed("single").c_str());
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay2.writes, 0);
  ASSERT_EQ(engine.delays.size(), 1u);
  EXPECT_EQ(engine.delays[0].ms, 500u);
  EXPECT_TRUE(rule->is_running());
  ASSERT_TRUE(engine.fire_next());
  EXPECT_EQ(e.relay2.writes, 1);
  EXPECT_FALSE(rule->is_running());
}

TEST_F(Runtime, SingleIgnoresATriggerWhileRunning) {
  auto rule = build_rule(engine, delayed("single").c_str());
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, true);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 1);
  ASSERT_EQ(engine.delays.size(), 1u);
  engine.fire_next();
  EXPECT_EQ(e.relay2.writes, 1);
  // Free again.
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 2);
}

// The edge single ignores still hands its state to a follow that has not played yet.
TEST_F(Runtime, SingleKeepsTheLastStateForAFollowStillToPlay) {
  auto rule =
      build_rule(engine, R"({"name":"Late","triggers":[{"source":"input","type":"state_change","object_id":"in_1"}],
      "actions":[{"source":"delay","delay_ms":500},{"source":"switch","type":"follow","object_id":"relay_2"}]})");
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, true);
  rule->on_binary_sensor(&e.in1, false);
  ASSERT_EQ(engine.delays.size(), 1u);
  ASSERT_TRUE(engine.fire_next());
  EXPECT_FALSE(e.relay2.state);
  EXPECT_EQ(e.relay2.writes, 1);
}

TEST_F(Runtime, RestartDropsThePendingDelay) {
  auto rule = build_rule(engine, delayed("restart").c_str());
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, true);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 2);
  ASSERT_EQ(engine.delays.size(), 1u);
  engine.fire_next();
  EXPECT_FALSE(engine.fire_next());
  EXPECT_EQ(e.relay2.writes, 1);
}

TEST_F(Runtime, ParallelRunsCopiesUpToEight) {
  auto rule = build_rule(engine, delayed("parallel").c_str());
  ASSERT_NE(rule, nullptr);
  for (int i = 0; i < 10; i++)
    rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 8);
  EXPECT_EQ(engine.delays.size(), 8u);
  while (engine.fire_next()) {
  }
  EXPECT_EQ(e.relay2.writes, 8);
  EXPECT_FALSE(rule->is_running());
}

TEST_F(Runtime, DisablingDropsThePendingDelay) {
  auto rule = build_rule(engine, delayed("single").c_str());
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, true);
  rule->set_enabled(false);
  EXPECT_FALSE(rule->is_running());
  EXPECT_TRUE(engine.delays.empty());
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 1);
  rule->set_enabled(true);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(Runtime, DestroyingARuleCancelsItsDelay) {
  auto rule = build_rule(engine, delayed("single").c_str());
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, true);
  ASSERT_EQ(engine.delays.size(), 1u);
  rule.reset();
  EXPECT_TRUE(engine.delays.empty());
}

}  // namespace esphome::automations::testing

namespace esphome::automations::testing {

TEST_F(Runtime, RestartFromInsideItsOwnActionStartsOver) {
  auto rule = build_rule(
      engine,
      R"({"name":"Self","mode":"restart","triggers":[{"source":"switch","type":"turn_on","object_id":"relay_1"}],
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},{"source":"delay","delay_ms":500},
                 {"source":"switch","type":"turn_on","object_id":"relay_2"}]})");
  ASSERT_NE(rule, nullptr);
  // Turning relay 1 on is the rule's own trigger, as a subscription would deliver it.
  e.relay1.on_change = [&]() { rule->on_switch(&e.relay1, true); };
  rule->on_switch(&e.relay1, true);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay2.writes, 0);  // the restarted run is still in its delay
  ASSERT_EQ(engine.delays.size(), 1u);
  EXPECT_TRUE(rule->is_running());
  engine.fire_next();
  EXPECT_EQ(e.relay2.writes, 1);
  EXPECT_FALSE(rule->is_running());
}

TEST_F(Runtime, ConditionThresholdsReadLikeTheTriggers) {
  ConditionConfig above_config, range_config;
  ASSERT_TRUE(
      load(R"({"type":"temperature","object_id":"temp","temperature_type":"above","threshold":25})", above_config));
  ASSERT_TRUE(load(
      R"({"type":"temperature","object_id":"temp","temperature_type":"range","min_threshold":10,"max_threshold":20})",
      range_config));
  CompiledCondition above, range;
  ASSERT_TRUE(compile_condition(above_config, above));
  ASSERT_TRUE(compile_condition(range_config, range));
  e.temp.state = 25;
  EXPECT_FALSE(above.check());
  e.temp.state = 25.5;
  EXPECT_TRUE(above.check());
  e.temp.state = 20;
  EXPECT_TRUE(range.check());
  e.temp.state = 20.5;
  EXPECT_FALSE(range.check());
}

TEST_F(Runtime, ClickIsAPressBetween200And1000Ms) {
  auto rule = build_rule(engine, R"({"name":"Click","triggers":[{"source":"input","type":"click","object_id":"in_1"}],
      "actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  auto press_for = [&](uint32_t start, uint32_t length) {
    engine.ms = start;
    rule->on_binary_sensor(&e.in1, true);
    engine.ms = start + length;
    rule->on_binary_sensor(&e.in1, false);
  };
  press_for(0, 199);
  EXPECT_EQ(e.relay1.writes, 0);
  press_for(1000, 200);
  EXPECT_EQ(e.relay1.writes, 1);
  press_for(2000, 1000);
  EXPECT_EQ(e.relay1.writes, 2);
  press_for(4000, 1001);
  EXPECT_EQ(e.relay1.writes, 2);
  // A release with no press before it is not a click.
  engine.ms = 6000;
  rule->on_binary_sensor(&e.in1, false);
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(Runtime, ClickSurvivesTheMillisWrap) {
  auto rule = build_rule(engine, R"({"name":"Click","triggers":[{"source":"input","type":"click","object_id":"in_1"}],
      "actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  engine.ms = UINT32_MAX - 100;
  rule->on_binary_sensor(&e.in1, true);
  engine.ms = 200;
  rule->on_binary_sensor(&e.in1, false);
  EXPECT_EQ(e.relay1.writes, 1);
}

// --- run now ---

TEST_F(Runtime, RunNowSkipsTheTriggersAndTheCondition) {
  auto rule = build_rule(engine, R"({"name":"Gated","triggers":[{"source":"input","type":"press","object_id":"in_1"}],
      "condition":{"type":"input","object_id":"in_2","state":"true"},
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}],
      "else_actions":[{"source":"switch","type":"turn_on","object_id":"relay_2"}]})");
  ASSERT_NE(rule, nullptr);
  ASSERT_FALSE(e.in2.state);
  EXPECT_TRUE(rule->run_now());
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay2.writes, 0);
  EXPECT_FALSE(rule->is_running());
}

TEST_F(Runtime, RunNowPlaysADisabledRule) {
  auto rule = build_rule(engine, delayed("single").c_str());
  ASSERT_NE(rule, nullptr);
  rule->set_enabled(false);
  EXPECT_TRUE(rule->run_now());
  EXPECT_TRUE(e.relay1.state);
  EXPECT_TRUE(rule->is_running());
  ASSERT_TRUE(engine.fire_next());
  EXPECT_EQ(e.relay2.writes, 1);
  // Its triggers stay off.
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 1);
}

TEST_F(Runtime, RunNowCarriesNoStateForAFollow) {
  auto rule = build_rule(
      engine, R"({"name":"Follow","triggers":[{"source":"switch","type":"state_change","object_id":"relay_1"}],
      "actions":[{"source":"switch","type":"follow","object_id":"relay_2"},
                 {"source":"switch","type":"turn_on","object_id":"boiler"}]})");
  ASSERT_NE(rule, nullptr);
  EXPECT_TRUE(rule->run_now());
  EXPECT_EQ(e.relay2.writes, 0);
  EXPECT_TRUE(e.boiler.state);  // the rest of the run plays
}

// A trigger single ignores while a run now plays hands it its state, as it would to any run.
TEST_F(Runtime, RunNowTakesTheStateOfATriggerThatArrivesWhileItPlays) {
  auto rule =
      build_rule(engine, R"({"name":"Late","triggers":[{"source":"input","type":"state_change","object_id":"in_1"}],
      "actions":[{"source":"delay","delay_ms":500},{"source":"switch","type":"follow","object_id":"relay_2"}]})");
  ASSERT_NE(rule, nullptr);
  EXPECT_TRUE(rule->run_now());
  rule->on_binary_sensor(&e.in1, true);
  ASSERT_EQ(engine.delays.size(), 1u);
  ASSERT_TRUE(engine.fire_next());
  EXPECT_TRUE(e.relay2.state);
}

TEST_F(Runtime, RunNowInSingleModeIsRefusedWhileARunIsUnderWay) {
  auto rule = build_rule(engine, delayed("single").c_str());
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_FALSE(rule->run_now());
  EXPECT_EQ(e.relay1.writes, 1);
  ASSERT_EQ(engine.delays.size(), 1u);
  // And a run now holds off a trigger the same way.
  engine.fire_next();
  EXPECT_TRUE(rule->run_now());
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 2);
  EXPECT_EQ(engine.delays.size(), 1u);
}

TEST_F(Runtime, RunNowInRestartModeStartsOver) {
  auto rule = build_rule(engine, delayed("restart").c_str());
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_TRUE(rule->run_now());
  EXPECT_EQ(e.relay1.writes, 2);
  ASSERT_EQ(engine.delays.size(), 1u);
  engine.fire_next();
  EXPECT_EQ(e.relay2.writes, 1);
  EXPECT_FALSE(rule->is_running());
}

TEST_F(Runtime, RunNowInParallelModeAddsACopyUpToEight) {
  auto rule = build_rule(engine, delayed("parallel").c_str());
  ASSERT_NE(rule, nullptr);
  rule->on_binary_sensor(&e.in1, true);
  for (int i = 0; i < 7; i++)
    EXPECT_TRUE(rule->run_now()) << i;
  EXPECT_FALSE(rule->run_now());
  EXPECT_EQ(e.relay1.writes, 8);
  EXPECT_EQ(engine.delays.size(), 8u);
  while (engine.fire_next()) {
  }
  EXPECT_EQ(e.relay2.writes, 8);
  EXPECT_TRUE(rule->run_now());
}

}  // namespace esphome::automations::testing
