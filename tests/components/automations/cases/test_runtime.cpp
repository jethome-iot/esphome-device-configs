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
  EXPECT_EQ(build_rule(engine, R"({"name":"Orphan switch","triggers":[{"source":"startup"}],
      "condition":{"type":"switch","object_id":"no_such"}})"),
            nullptr);
}

// A config switch is the device's own setting: a rule naming it, as its action, its trigger or
// its condition, is refused as one naming a switch that is not there, and the same rule on a
// relay builds.
TEST_F(Runtime, BuildRefusesAConfigSwitchAsAMissingOne) {
  static const char *const UNLOCK =
      R"({"name":"Unlock","triggers":[{"source":"startup"}],"actions":[{"source":"switch","type":"turn_on","object_id":"%s"}]})";
  static const char *const WATCH =
      R"({"name":"Watch","triggers":[{"source":"switch","type":"turn_on","object_id":"%s"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})";
  static const char *const GATE =
      R"({"name":"Gate","triggers":[{"source":"startup"}],"condition":{"type":"switch","object_id":"%s"},"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})";
  auto naming = [](std::string json, const char *object_id) {
    json.replace(json.find("%s"), 2, object_id);
    AutomationConfig config;
    EXPECT_TRUE(load(json.c_str(), config));
    return config;
  };
  for (const char *object_id : {"config_switch", "no_such"}) {
    std::string error;
    EXPECT_EQ(RuntimeAutomation::build(&engine, naming(UNLOCK, object_id), &error), nullptr) << object_id;
    EXPECT_EQ(error, "Action 1: switch not found") << object_id;
    EXPECT_EQ(RuntimeAutomation::build(&engine, naming(WATCH, object_id), &error), nullptr) << object_id;
    EXPECT_EQ(error, "Trigger 1: switch not found") << object_id;
    EXPECT_EQ(RuntimeAutomation::build(&engine, naming(GATE, object_id), &error), nullptr) << object_id;
    EXPECT_EQ(error, "Condition: switch not found") << object_id;
  }

  auto unlock = RuntimeAutomation::build(&engine, naming(UNLOCK, "relay_2"));
  ASSERT_NE(unlock, nullptr);
  unlock->on_startup();
  EXPECT_TRUE(e.relay2.state);
  auto watch = RuntimeAutomation::build(&engine, naming(WATCH, "relay_2"));
  ASSERT_NE(watch, nullptr);
  watch->on_switch(&e.relay2, true);
  EXPECT_TRUE(e.relay1.state);
  e.relay1.turn_off();
  auto gate = RuntimeAutomation::build(&engine, naming(GATE, "relay_2"));
  ASSERT_NE(gate, nullptr);
  gate->on_startup();
  EXPECT_TRUE(e.relay1.state);
}

TEST_F(Runtime, AConditionTriggerNeedsACondition) {
  for (const char *json : {R"({"name":"Nothing to watch","triggers":[{"source":"condition"}],
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})",
                           R"({"name":"Nothing to watch","triggers":[{"source":"startup"},{"source":"condition"}],
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})"}) {
    AutomationConfig config;
    ASSERT_TRUE(load(json, config));
    std::string error;
    EXPECT_EQ(RuntimeAutomation::build(&engine, config, &error), nullptr);
    EXPECT_EQ(error, "Trigger " + std::to_string(config.triggers.size()) + ": no condition to watch");
  }
}

static const char *const MIRROR = R"({"name":"Mirror","triggers":[{"source":"condition"}],
    "condition":{"type":"input","object_id":"in_1"},
    "actions":[{"source":"switch","type":"follow","object_id":"relay_1"}],
    "else_actions":[{"source":"switch","type":"follow","object_id":"relay_1"},
                    {"source":"switch","type":"follow","object_id":"relay_2","invert":true}]})";

TEST_F(Runtime, AConditionTriggerWaitsForTheStartupEvent) {
  auto rule = build_rule(engine, MIRROR);
  ASSERT_NE(rule, nullptr);
  EXPECT_TRUE(rule->watches_condition());
  EXPECT_FALSE(rule->is_watching());
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 0);

  rule->on_startup();
  EXPECT_TRUE(rule->is_watching());
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 1);
  rule->on_startup();  // watching already: nothing new to say
  EXPECT_EQ(e.relay1.writes, 1);
}

// The run carries the result: a follow under either branch copies it.
TEST_F(Runtime, FollowMirrorsTheConditionResult) {
  auto rule = build_rule(engine, MIRROR);
  ASSERT_NE(rule, nullptr);
  rule->watch_condition();
  EXPECT_FALSE(e.relay1.state);
  EXPECT_TRUE(e.relay2.state);  // the else branch carries false, inverted
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay2.writes, 1);
  e.in1.publish_state(false);
  rule->on_binary_sensor(&e.in1, false);
  EXPECT_FALSE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 3);
}

// An input whose inversion flipped re-emits its state as a level: no press, but a new state
// for whatever reads it.
TEST_F(Runtime, AConditionTriggerSeesALevel) {
  auto rule = build_rule(engine, MIRROR);
  ASSERT_NE(rule, nullptr);
  rule->watch_condition();
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true, true);
  EXPECT_TRUE(e.relay1.state);
}

// Disabling ends the watch; enabling leaves the new start to the engine.
TEST_F(Runtime, AConditionTriggerIsQuietWhileDisabled) {
  auto rule = build_rule(engine, MIRROR);
  ASSERT_NE(rule, nullptr);
  rule->watch_condition();
  EXPECT_EQ(e.relay1.writes, 1);
  rule->set_enabled(false);
  EXPECT_FALSE(rule->is_watching());
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 1);
  rule->watch_condition();  // a disabled rule does not start watching
  EXPECT_EQ(e.relay1.writes, 1);

  rule->set_enabled(true);
  EXPECT_EQ(e.relay1.writes, 1);
  rule->watch_condition();
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(Runtime, AStartupRuleWithNoConditionTriggerWatchesNothing) {
  auto rule = build_rule(engine, R"({"name":"Gate","triggers":[{"source":"startup"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  EXPECT_FALSE(rule->watches_condition());
  rule->on_startup();
  EXPECT_FALSE(rule->is_watching());
  EXPECT_EQ(e.relay1.writes, 0);  // in_1 is off: the else branch is empty
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 0);
}

// A thermostat let go of a relay: only the branch for the current result, and only when it
// drives that relay, plays again.
TEST_F(Runtime, ARelayLetGoIsSetByTheBranchThatDrivesIt) {
  auto rule = build_rule(engine, R"({"name":"Half","triggers":[{"source":"condition"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}],
      "else_actions":[{"source":"switch","type":"toggle","object_id":"relay_2"}]})");
  ASSERT_NE(rule, nullptr);
  rule->on_released(&e.relay1);  // not watching yet
  EXPECT_EQ(e.relay2.writes, 0);
  rule->watch_condition();
  EXPECT_EQ(e.relay2.writes, 1);
  rule->on_released(&e.relay1);  // false: the else branch does not drive relay_1
  rule->on_released(&e.boiler);
  rule->on_released(&e.relay2);  // a toggle sets nothing: it would flip the relay on every let-go
  EXPECT_EQ(e.relay2.writes, 1);
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_EQ(e.relay1.writes, 1);
  e.relay1.turn_off();
  rule->on_released(&e.relay1);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 3);
}

// A relay let go goes where the branch leaves it, its last step on it, a follow carrying the
// result; the rest of the branch, delays included, does not play again.
TEST_F(Runtime, ARelayLetGoGoesWhereTheBranchLeavesIt) {
  auto rule = build_rule(engine, R"({"name":"Pulse","triggers":[{"source":"condition"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_2"},
                 {"source":"switch","type":"turn_on","object_id":"relay_1"},
                 {"source":"delay","delay_ms":500},
                 {"source":"switch","type":"turn_off","object_id":"relay_1"}],
      "else_actions":[{"source":"switch","type":"follow","object_id":"relay_1","invert":true}]})");
  ASSERT_NE(rule, nullptr);
  rule->watch_condition();
  EXPECT_TRUE(e.relay1.state);  // false, inverted
  e.relay1.turn_off();
  rule->on_released(&e.relay1);
  EXPECT_TRUE(e.relay1.state);
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true);
  ASSERT_TRUE(engine.fire_next());
  EXPECT_FALSE(e.relay1.state);
  const int relay2_writes = e.relay2.writes;
  e.relay1.turn_on();
  rule->on_released(&e.relay1);
  EXPECT_FALSE(e.relay1.state);
  EXPECT_EQ(e.relay2.writes, relay2_writes);
  EXPECT_TRUE(engine.delays.empty());
}

// A toggle after a step that sets the relay turns that over: the relay goes where the branch,
// played through, leaves it, during the run and after it.
TEST_F(Runtime, ARelayLetGoTakesAToggleAfterASetStep) {
  auto rule = build_rule(engine, R"({"name":"Blink","triggers":[{"source":"condition"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},
                 {"source":"delay","delay_ms":500},
                 {"source":"switch","type":"toggle","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  rule->watch_condition();
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true);
  e.relay1.turn_off();  // where a thermostat left it, the delay still running
  rule->on_released(&e.relay1);
  EXPECT_TRUE(e.relay1.state);
  ASSERT_TRUE(engine.fire_next());
  EXPECT_FALSE(e.relay1.state);
  e.relay1.turn_on();
  rule->on_released(&e.relay1);
  EXPECT_FALSE(e.relay1.state);
}

// While a run is under way the relay goes where the run has got to: a delay before its step on
// the relay still holds, and what the run set before the delay is set again.
TEST_F(Runtime, ARelayLetGoDuringARunGoesWhereTheRunHasGot) {
  auto later = build_rule(engine, R"({"name":"Later","triggers":[{"source":"condition"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"delay","delay_ms":500},
                 {"source":"switch","type":"turn_on","object_id":"relay_1"}]})");
  ASSERT_NE(later, nullptr);
  later->watch_condition();
  e.in1.publish_state(true);
  later->on_binary_sensor(&e.in1, true);
  later->on_released(&e.relay1);
  EXPECT_FALSE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 0);
  ASSERT_TRUE(engine.fire_next());
  EXPECT_TRUE(e.relay1.state);
}

TEST_F(Runtime, ARelayLetGoDuringARunKeepsWhatTheRunSet) {
  auto pulse = build_rule(engine, R"({"name":"Pulse off","triggers":[{"source":"condition"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"switch","type":"turn_off","object_id":"relay_1"},
                 {"source":"delay","delay_ms":500},
                 {"source":"switch","type":"turn_on","object_id":"relay_1"},
                 {"source":"switch","type":"toggle","object_id":"relay_2"}]})");
  ASSERT_NE(pulse, nullptr);
  pulse->watch_condition();
  e.relay1.turn_on();  // where a thermostat left it
  e.in1.publish_state(true);
  pulse->on_binary_sensor(&e.in1, true);
  EXPECT_FALSE(e.relay1.state);
  e.relay1.turn_on();
  pulse->on_released(&e.relay1);
  EXPECT_FALSE(e.relay1.state);  // off for the delay, as the run left it
  EXPECT_EQ(e.relay2.writes, 0);
  ASSERT_TRUE(engine.fire_next());
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay2.writes, 1);
}

// In single, a change during the run is handed to the follows still to play; the ones the run has
// passed keep the state they played with, and a relay let go meanwhile goes where they left it.
TEST_F(Runtime, ARelayLetGoDuringASingleRunKeepsWhatItsPassedFollowsSet) {
  auto rule = build_rule(engine, R"({"name":"Hold","mode":"single","triggers":[{"source":"condition"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"switch","type":"follow","object_id":"relay_1"},
                 {"source":"delay","delay_ms":500},
                 {"source":"switch","type":"follow","object_id":"relay_1"}],
      "else_actions":[{"source":"switch","type":"follow","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  rule->watch_condition();
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_TRUE(e.relay1.state);
  e.in1.publish_state(false);
  rule->on_binary_sensor(&e.in1, false);  // single: the run goes on, its last follow copies false
  e.relay1.turn_off();                    // where a thermostat left it
  rule->on_released(&e.relay1);
  EXPECT_TRUE(e.relay1.state);  // the first follow played with true
  ASSERT_TRUE(engine.fire_next());
  EXPECT_FALSE(e.relay1.state);
}

// Steps the run has passed on one relay add up: a toggle turns over the step before it, and one
// with nothing before it leaves the relay alone.
TEST_F(Runtime, ARelayLetGoDuringARunAddsUpItsPassedSteps) {
  auto rule = build_rule(engine, R"({"name":"Steps","triggers":[{"source":"condition"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},
                 {"source":"switch","type":"toggle","object_id":"relay_1"},
                 {"source":"switch","type":"toggle","object_id":"relay_2"},
                 {"source":"delay","delay_ms":500},
                 {"source":"switch","type":"turn_on","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  rule->watch_condition();
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true);
  e.relay1.turn_on();  // where a thermostat left it
  rule->on_released(&e.relay1);
  EXPECT_FALSE(e.relay1.state);  // turned on, then over
  const int relay2_writes = e.relay2.writes;
  rule->on_released(&e.relay2);  // a toggle with nothing before it
  EXPECT_EQ(e.relay2.writes, relay2_writes);
  ASSERT_TRUE(engine.fire_next());
  EXPECT_TRUE(e.relay1.state);
}

// In parallel the run for the result may have ended while an older one waits in a delay: the
// relay goes where the branch for the result leaves it, not where the older run has got to.
TEST_F(Runtime, ARelayLetGoInParallelFollowsTheResultNotAnOlderRun) {
  auto rule = build_rule(engine, R"({"name":"Both","mode":"parallel","triggers":[{"source":"condition"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},
                 {"source":"delay","delay_ms":500},
                 {"source":"switch","type":"turn_on","object_id":"relay_2"}],
      "else_actions":[{"source":"switch","type":"turn_off","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  rule->watch_condition();
  e.in1.publish_state(true);
  rule->on_binary_sensor(&e.in1, true);  // a run waits in its delay
  e.in1.publish_state(false);
  rule->on_binary_sensor(&e.in1, false);  // the else run plays and ends
  EXPECT_FALSE(e.relay1.state);
  e.relay1.turn_on();  // where a thermostat left it
  rule->on_released(&e.relay1);
  EXPECT_FALSE(e.relay1.state);
  ASSERT_TRUE(engine.fire_next());
  EXPECT_TRUE(e.relay2.state);
}

// In parallel two runs can wait in their delays at once: the relay goes where the one started last
// has got to.
TEST_F(Runtime, ARelayLetGoInParallelTakesTheRunStartedLast) {
  auto rule = build_rule(engine, R"({"name":"Both","mode":"parallel","triggers":[{"source":"condition"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},
                 {"source":"delay","delay_ms":500},
                 {"source":"switch","type":"turn_off","object_id":"relay_1"}],
      "else_actions":[{"source":"switch","type":"turn_off","object_id":"relay_1"},
                      {"source":"delay","delay_ms":500},
                      {"source":"switch","type":"turn_on","object_id":"relay_1"}]})");
  ASSERT_NE(rule, nullptr);
  e.in1.publish_state(true);
  rule->watch_condition();  // the then run waits in its delay, relay_1 on
  e.in1.publish_state(false);
  rule->on_binary_sensor(&e.in1, false);  // the else run waits in its delay too, relay_1 off
  EXPECT_EQ(engine.delays.size(), 2u);
  e.relay1.turn_on();  // where a thermostat left it
  rule->on_released(&e.relay1);
  EXPECT_FALSE(e.relay1.state);
}

// A change of the condition the rule has not been handed yet is not taken as acted on when a
// relay is let go: it fires when it arrives.
TEST_F(Runtime, ARelayLetGoLeavesAChangeOnItsWayToTheRule) {
  auto rule = build_rule(engine, R"({"name":"Half","triggers":[{"source":"condition"}],
      "condition":{"type":"input","object_id":"in_1"},
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}],
      "else_actions":[{"source":"switch","type":"toggle","object_id":"relay_2"}]})");
  ASSERT_NE(rule, nullptr);
  rule->watch_condition();
  EXPECT_EQ(e.relay2.writes, 1);
  e.in1.publish_state(true);  // not handed to the rule yet
  rule->on_released(&e.boiler);
  rule->on_released(&e.relay2);  // what the old result drove, not the new one
  EXPECT_EQ(e.relay1.writes, 0);
  EXPECT_EQ(e.relay2.writes, 1);
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_TRUE(e.relay1.state);
}

TEST_F(Runtime, SwitchConditionReadsTheSwitch) {
  ConditionConfig config;
  ASSERT_TRUE(load(R"({"type":"switch","object_id":"relay_1","state":"false"})", config));
  CompiledCondition c;
  ASSERT_TRUE(compile_condition(config, c));
  EXPECT_TRUE(c.check());
  e.relay1.turn_on();
  EXPECT_FALSE(c.check());
  e.relay1.turn_off();
  EXPECT_TRUE(c.check());
}

// An interlock: In 1 turns relay 2 on only while relay 1 is off, and the else branch says so.
static const char *const INTERLOCK = R"({"name":"Interlock",
    "triggers":[{"source":"input","type":"press","object_id":"in_1"}],
    "condition":{"type":"switch","object_id":"relay_1","state":"false"},
    "actions":[{"source":"switch","type":"turn_on","object_id":"relay_2"}],
    "else_actions":[{"source":"switch","type":"turn_off","object_id":"relay_2"}]})";

TEST_F(Runtime, ASwitchConditionInterlocksTwoRelays) {
  auto rule = build_rule(engine, INTERLOCK);
  ASSERT_NE(rule, nullptr);
  e.relay1.turn_on();
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_FALSE(e.relay2.state);
  EXPECT_EQ(e.relay2.writes, 1);

  e.relay1.turn_off();
  rule->on_binary_sensor(&e.in1, true);
  EXPECT_TRUE(e.relay2.state);
}

// A rule may read the switch its own trigger watches: the condition sees the state the switch
// is in now, the one the trigger carries.
static const char *const OWN_SWITCH = R"({"name":"Echo",
    "triggers":[{"source":"switch","type":"state_change","object_id":"relay_1"}],
    "condition":{"type":"switch","object_id":"relay_1"},
    "actions":[{"source":"switch","type":"turn_on","object_id":"relay_2"}],
    "else_actions":[{"source":"switch","type":"turn_off","object_id":"relay_2"}]})";

TEST_F(Runtime, ASwitchConditionReadsTheStateItsTriggerCarries) {
  auto rule = build_rule(engine, OWN_SWITCH);
  ASSERT_NE(rule, nullptr);
  e.relay1.turn_on();
  rule->on_switch(&e.relay1, true);
  EXPECT_TRUE(e.relay2.state);
  e.relay1.turn_off();
  rule->on_switch(&e.relay1, false);
  EXPECT_FALSE(e.relay2.state);
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
