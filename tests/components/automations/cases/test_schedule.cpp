#include "common.h"

namespace esphome::automations::testing {

// 2026-02-02 00:00:00 UTC, a Monday; `day` counts from it.
static const time_t MONDAY = 1769990400;
static time_t at(int day, int hour, int minute, int second = 0) {
  return MONDAY + day * 86400 + hour * 3600 + minute * 60 + second;
}

static bool load_schedule(const std::string &windows, TriggerConfig &out) {
  const std::string json = R"({"source":"schedule","windows":)" + windows + "}";
  return load(json.c_str(), out);
}

static std::string round_trip(const std::string &windows) {
  TriggerConfig t;
  if (!load_schedule(windows, t))
    return "<refused>";
  return dump(t);
}

// --- The file format ---

TEST(ScheduleConfig, WritesTheDaysOutInFullMondayFirst) {
  EXPECT_EQ(
      round_trip(
          R"([{"days":["fri","mon","tue","wed","thu"],"from":"08:00","to":"20:00"},{"days":["sun","sat"],"from":"10:00","to":"16:00"}])"),
      R"({"source":"schedule","windows":[{"days":["mon","tue","wed","thu","fri"],"from":"08:00","to":"20:00"},)"
      R"({"days":["sat","sun"],"from":"10:00","to":"16:00"}]})");
  EXPECT_EQ(round_trip(R"([{"days":["sun","mon","sun"],"from":"00:00","to":"24:00"}])"),
            R"({"source":"schedule","windows":[{"days":["mon","sun"],"from":"00:00","to":"24:00"}]})");
}

TEST(ScheduleConfig, NoDaysMeansEveryDay) {
  TriggerConfig t;
  ASSERT_TRUE(load_schedule(R"([{"from":"22:00","to":"06:30"}])", t));
  ASSERT_EQ(t.schedule_windows.size(), 1u);
  EXPECT_EQ(t.schedule_windows[0].days, ScheduleWindow::EVERY_DAY);
  EXPECT_EQ(t.schedule_windows[0].from, 22 * 60);
  EXPECT_EQ(t.schedule_windows[0].to, 6 * 60 + 30);
  EXPECT_EQ(dump(t), R"({"source":"schedule","windows":[{"days":["mon","tue","wed","thu","fri","sat","sun"],)"
                     R"("from":"22:00","to":"06:30"}]})");
}

TEST(ScheduleConfig, MidnightEndsAWindowAsEither) {
  TriggerConfig t;
  ASSERT_TRUE(load_schedule(R"([{"from":"18:00","to":"00:00"},{"from":"18:00","to":"24:00"}])", t));
  EXPECT_EQ(t.schedule_windows[0].to, 0);
  EXPECT_EQ(t.schedule_windows[1].to, 24 * 60);
  EXPECT_NE(dump(t).find(R"("to":"00:00")"), std::string::npos);
  EXPECT_NE(dump(t).find(R"("to":"24:00")"), std::string::npos);
}

TEST(ScheduleConfig, ReadsAgainFromScratch) {
  TriggerConfig t;
  ASSERT_TRUE(load_schedule(R"([{"from":"08:00","to":"09:00"},{"from":"10:00","to":"11:00"}])", t));
  ASSERT_TRUE(load_schedule(R"([{"from":"12:00","to":"13:00"}])", t));
  ASSERT_EQ(t.schedule_windows.size(), 1u);
  EXPECT_EQ(t.schedule_windows[0].from, 12 * 60);
}

TEST(ScheduleConfig, RefusesWhatItCannotRead) {
  for (const char *windows : {
           R"([])",
           R"({})",
           R"("08:00-20:00")",
           R"(["mon 08:00-20:00"])",
           R"([{"from":"08:00"}])",
           R"([{"to":"20:00"}])",
           R"([{"from":800,"to":2000}])",
           R"([{"from":"8:00","to":"20:00"}])",
           R"([{"from":" 8:00","to":"20:00"}])",
           R"([{"from":"08:0","to":"20:00"}])",
           R"([{"from":"08-00","to":"20:00"}])",
           R"([{"from":"08:00:00","to":"20:00"}])",
           R"([{"from":"ab:cd","to":"20:00"}])",
           R"([{"from":"0x:00","to":"20:00"}])",
           R"([{"from":"08:x0","to":"20:00"}])",
           R"([{"from":"08:00","to":"20:0x"}])",
           R"([{"from":"12:60","to":"13:00"}])",
           R"([{"from":"24:00","to":"06:00"}])",
           R"([{"from":"08:00","to":"24:01"}])",
           R"([{"from":"08:00","to":"25:00"}])",
           R"([{"from":"08:00","to":"08:00"}])",
           R"([{"from":"00:00","to":"00:00"}])",
           R"([{"days":[],"from":"08:00","to":"20:00"}])",
           R"([{"days":"mon","from":"08:00","to":"20:00"}])",
           R"([{"days":["Mon"],"from":"08:00","to":"20:00"}])",
           R"([{"days":["monday"],"from":"08:00","to":"20:00"}])",
           R"([{"days":[1],"from":"08:00","to":"20:00"}])",
           // One bad window refuses the trigger, not just itself.
           R"([{"from":"08:00","to":"20:00"},{"from":"09:00","to":"09:00"}])",
       }) {
    TriggerConfig t;
    EXPECT_FALSE(load_schedule(windows, t)) << windows;
  }
  TriggerConfig t;
  EXPECT_FALSE(load(R"({"source":"schedule"})", t));
}

TEST(ScheduleConfig, DescribesAWindowForTheLog) {
  TriggerConfig t;
  ASSERT_TRUE(
      load_schedule(R"([{"days":["sat","mon"],"from":"22:00","to":"06:00"},{"from":"00:00","to":"24:00"}])", t));
  EXPECT_EQ(t.schedule_windows[0].describe(), "mon,sat 22:00-06:00");
  EXPECT_EQ(t.schedule_windows[1].describe(), "mon,tue,wed,thu,fri,sat,sun 00:00-24:00");
}

// --- Inside or outside ---

class ScheduleOn : public ::testing::Test {
 protected:
  void SetUp() override { engine.with_clock(); }
  // Whether the windows hold `when`.
  bool on(const char *windows, time_t when) {
    TriggerConfig config;
    CompiledTrigger trigger;
    if (!load_schedule(windows, config) || !compile_trigger(&engine, config, trigger)) {
      ADD_FAILURE() << windows;
      return false;
    }
    return trigger.schedule_on(ESPTime::from_epoch_utc(when));
  }
  FakeEngine engine;
};

TEST(ScheduleTrigger, NeedsAClock) {
  FakeEngine engine;
  TriggerConfig config;
  ASSERT_TRUE(load_schedule(R"([{"from":"08:00","to":"20:00"}])", config));
  CompiledTrigger trigger;
  EXPECT_FALSE(compile_trigger(&engine, config, trigger));
  engine.with_clock();
  EXPECT_TRUE(compile_trigger(&engine, config, trigger));
  EXPECT_FALSE(trigger.schedule_on(ESPTime{}));
}

TEST(ScheduleTrigger, RefusesAWindowTheFileCouldNotHold) {
  FakeEngine engine;
  engine.with_clock();
  TriggerConfig config;
  config.source = SourceTrigger::SCHEDULE;
  CompiledTrigger trigger;
  EXPECT_FALSE(compile_trigger(&engine, config, trigger));
  for (const ScheduleWindow &window :
       {ScheduleWindow{0, 480, 1200}, ScheduleWindow{0x80, 480, 1200},
        ScheduleWindow{ScheduleWindow::EVERY_DAY, 480, 480}, ScheduleWindow{ScheduleWindow::EVERY_DAY, 1440, 60},
        ScheduleWindow{ScheduleWindow::EVERY_DAY, 480, 1441}}) {
    config.schedule_windows = {window};
    EXPECT_FALSE(compile_trigger(&engine, config, trigger)) << window.describe();
  }
  config.schedule_windows = {ScheduleWindow{ScheduleWindow::EVERY_DAY, 0, 1440}};
  EXPECT_TRUE(compile_trigger(&engine, config, trigger));
}

TEST_F(ScheduleOn, FromItsStartUpToItsEnd) {
  const char *weekdays = R"([{"days":["mon","tue","wed","thu","fri"],"from":"08:00","to":"20:00"}])";
  EXPECT_FALSE(on(weekdays, at(0, 7, 59, 59)));
  EXPECT_TRUE(on(weekdays, at(0, 8, 0)));
  EXPECT_TRUE(on(weekdays, at(4, 19, 59, 59)));
  EXPECT_FALSE(on(weekdays, at(4, 20, 0)));
  EXPECT_FALSE(on(weekdays, at(5, 12, 0)));
}

TEST_F(ScheduleOn, PastMidnightBelongsToTheDayItStarts) {
  const char *friday_night = R"([{"days":["fri"],"from":"22:00","to":"06:00"}])";
  EXPECT_FALSE(on(friday_night, at(4, 21, 59)));
  EXPECT_TRUE(on(friday_night, at(4, 22, 0)));
  EXPECT_TRUE(on(friday_night, at(5, 5, 59, 59)));
  EXPECT_FALSE(on(friday_night, at(5, 6, 0)));
  EXPECT_FALSE(on(friday_night, at(4, 5, 0)));  // Thursday did not start one
  EXPECT_FALSE(on(friday_night, at(5, 22, 0)));
}

TEST_F(ScheduleOn, RunsOverTheEndOfTheWeek) {
  EXPECT_TRUE(on(R"([{"days":["sat"],"from":"23:00","to":"01:00"}])", at(6, 0, 30)));
  EXPECT_TRUE(on(R"([{"days":["sun"],"from":"23:00","to":"01:00"}])", at(7, 0, 30)));
  EXPECT_FALSE(on(R"([{"days":["sun"],"from":"23:00","to":"01:00"}])", at(6, 0, 30)));
}

TEST_F(ScheduleOn, MidnightEndsTheDay) {
  const char *monday = R"([{"days":["mon"],"from":"00:00","to":"24:00"}])";
  EXPECT_TRUE(on(monday, at(0, 0, 0)));
  EXPECT_TRUE(on(monday, at(0, 23, 59, 59)));
  EXPECT_FALSE(on(monday, at(1, 0, 0)));
  EXPECT_FALSE(on(monday, at(-1, 23, 59, 59)));

  const char *evening = R"([{"days":["mon"],"from":"18:00","to":"00:00"}])";
  EXPECT_TRUE(on(evening, at(0, 23, 59, 59)));
  EXPECT_FALSE(on(evening, at(1, 0, 0)));
}

TEST_F(ScheduleOn, DaysThatMeetAtMidnightLeaveNoGap) {
  const char *night = R"([{"days":["mon"],"from":"18:00","to":"24:00"},{"days":["tue"],"from":"00:00","to":"06:00"}])";
  EXPECT_TRUE(on(night, at(0, 23, 59, 59)));
  EXPECT_TRUE(on(night, at(1, 0, 0)));
  EXPECT_FALSE(on(night, at(1, 6, 0)));
}

TEST_F(ScheduleOn, WindowsThatTouchLeaveNoGap) {
  const char *two = R"([{"from":"08:00","to":"12:00"},{"from":"12:00","to":"20:00"}])";
  EXPECT_TRUE(on(two, at(0, 11, 59, 59)));
  EXPECT_TRUE(on(two, at(0, 12, 0)));
  EXPECT_FALSE(on(two, at(0, 20, 0)));
}

// --- A rule driven by the one-second tick ---

static const char *const SHOP =
    R"({"name":"Shop","triggers":[{"source":"schedule","windows":[{"days":["mon","tue","wed","thu","fri"],"from":"08:00","to":"20:00"}]}],
        "actions":[{"source":"switch","type":"follow","object_id":"relay_1"}]})";

class ScheduleRule : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_entities();
    engine.with_clock();
  }
  void adopt(const char *json) {
    auto rule = build_rule(engine, json);
    ASSERT_NE(rule, nullptr);
    engine.adopt(std::move(rule));
  }
  // The clock set to `when`, and one tick there.
  void boot_at(time_t when) {
    engine.now = when;
    engine.tick();
  }
  // Every second up to `until`, as the engine's interval ticks.
  void run_until(time_t until) {
    while (engine.now < until) {
      engine.now++;
      engine.tick();
    }
  }
  FakeEngine engine;
  Entities &e = entities();
};

TEST_F(ScheduleRule, ABootInsideAWindowTurnsTheRelayOn) {
  adopt(SHOP);
  boot_at(at(0, 10, 0));
  EXPECT_TRUE(e.relay1.state);
  run_until(at(0, 10, 5));
  EXPECT_EQ(e.relay1.writes, 1);
}

TEST_F(ScheduleRule, ABootOutsideTurnsItOff) {
  adopt(SHOP);
  e.relay1.publish_state(true);  // what a restore mode may have brought back
  boot_at(at(0, 21, 0));
  EXPECT_FALSE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 1);
}

TEST_F(ScheduleRule, WaitsForAValidClock) {
  adopt(SHOP);
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 0);
  boot_at(at(0, 10, 0));
  EXPECT_TRUE(e.relay1.state);
}

TEST_F(ScheduleRule, EachEdgeFiresOnce) {
  adopt(SHOP);
  boot_at(at(0, 7, 59, 58));
  run_until(at(0, 8, 0, 2));
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 2);
  boot_at(at(0, 19, 59, 58));  // a jump ahead: the next tick carries on
  run_until(at(0, 20, 0, 2));
  EXPECT_FALSE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 3);
}

TEST_F(ScheduleRule, MissedSecondsAreCaughtUp) {
  adopt(SHOP);
  boot_at(at(0, 7, 59, 50));
  engine.now = at(0, 8, 0, 20);
  engine.tick();
  EXPECT_TRUE(e.relay1.state);
}

TEST_F(ScheduleRule, AClockThatJumpsIsFollowedAtOnce) {
  adopt(SHOP);
  boot_at(at(0, 7, 0));
  boot_at(at(0, 9, 0));
  EXPECT_TRUE(e.relay1.state);
  boot_at(at(0, 6, 0));
  EXPECT_FALSE(e.relay1.state);
  // A short step back, which cron waits out, here across the window's end.
  boot_at(at(0, 20, 5));
  boot_at(at(0, 19, 58));
  EXPECT_TRUE(e.relay1.state);
  run_until(at(0, 20, 0, 1));
  EXPECT_FALSE(e.relay1.state);
}

TEST_F(ScheduleRule, SchedulesInOneRuleActAsOne) {
  adopt(R"({"name":"Two","triggers":[
      {"source":"schedule","windows":[{"days":["mon","tue","wed","thu","fri"],"from":"08:00","to":"20:00"}]},
      {"source":"schedule","windows":[{"from":"18:00","to":"22:00"}]}],
      "actions":[{"source":"switch","type":"follow","object_id":"relay_1"}]})");
  boot_at(at(0, 10, 0));
  EXPECT_TRUE(e.relay1.state);
  boot_at(at(0, 19, 59, 58));
  run_until(at(0, 20, 0, 2));
  EXPECT_TRUE(e.relay1.state);
  boot_at(at(0, 21, 59, 58));
  run_until(at(0, 22, 0, 2));
  EXPECT_FALSE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(ScheduleRule, DaysThatMeetAtMidnightDoNotFlicker) {
  adopt(R"({"name":"Night","triggers":[{"source":"schedule","windows":[
      {"days":["mon"],"from":"18:00","to":"24:00"},{"days":["tue"],"from":"00:00","to":"06:00"}]}],
      "actions":[{"source":"switch","type":"follow","object_id":"relay_1"}]})");
  boot_at(at(0, 23, 59, 58));
  run_until(at(1, 0, 0, 2));
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 1);
}

TEST_F(ScheduleRule, AHandSwitchHoldsUntilTheNextEdge) {
  adopt(SHOP);
  boot_at(at(0, 10, 0));
  e.relay1.turn_off();
  run_until(at(0, 10, 10));
  EXPECT_FALSE(e.relay1.state);
  e.relay1.turn_on();
  boot_at(at(0, 19, 59, 58));
  run_until(at(0, 20, 0, 2));
  EXPECT_FALSE(e.relay1.state);
  boot_at(at(1, 7, 59, 58));
  run_until(at(1, 8, 0, 2));
  EXPECT_TRUE(e.relay1.state);
}

TEST_F(ScheduleRule, EnablingTheRuleAppliesItAgain) {
  adopt(SHOP);
  boot_at(at(0, 10, 0));
  engine.rule(0)->set_enabled(false);
  e.relay1.turn_off();
  run_until(at(0, 10, 10));
  EXPECT_FALSE(e.relay1.state);
  // Only a rule coming back from off hands the state over again.
  engine.rule(0)->set_enabled(true);
  engine.rule(0)->set_enabled(true);
  run_until(at(0, 10, 11));
  EXPECT_TRUE(e.relay1.state);
  e.relay1.turn_off();
  engine.rule(0)->set_enabled(true);
  run_until(at(0, 10, 12));
  EXPECT_FALSE(e.relay1.state);
}

TEST_F(ScheduleRule, ABusyRuleIsOfferedTheStateUntilItTakesIt) {
  adopt(R"({"name":"Busy","mode":"single","triggers":[{"source":"schedule","windows":[{"from":"08:00","to":"08:01"}]}],
      "actions":[{"source":"switch","type":"follow","object_id":"relay_1"},{"source":"delay","delay_ms":120000},
                 {"source":"switch","type":"turn_on","object_id":"relay_2"}]})");
  boot_at(at(0, 8, 0));
  EXPECT_TRUE(e.relay1.state);
  run_until(at(0, 8, 1, 5));
  EXPECT_TRUE(e.relay1.state);  // the run still waits in its delay
  ASSERT_TRUE(engine.fire_next());
  EXPECT_TRUE(e.relay2.state);
  run_until(at(0, 8, 1, 6));
  EXPECT_FALSE(e.relay1.state);
  ASSERT_TRUE(engine.fire_next());
  run_until(at(0, 8, 2));
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(ScheduleRule, AFullParallelRuleIsOfferedTheStateUntilASlotFrees) {
  adopt(R"({"name":"Full","mode":"parallel","triggers":[
      {"source":"schedule","windows":[{"from":"08:00","to":"08:01"}]},
      {"source":"input","type":"press","object_id":"in_1"}],
      "actions":[{"source":"switch","type":"follow","object_id":"relay_1"},{"source":"delay","delay_ms":120000}]})");
  boot_at(at(0, 8, 0));
  for (int i = 0; i < 7; i++) {
    engine.rule(0)->on_binary_sensor(&e.in1, true);
    engine.rule(0)->on_binary_sensor(&e.in1, false);
  }
  ASSERT_EQ(engine.delays.size(), 8u);
  run_until(at(0, 8, 1, 5));
  EXPECT_TRUE(e.relay1.state);  // all eight runs still wait
  ASSERT_TRUE(engine.fire_next());
  run_until(at(0, 8, 1, 6));
  EXPECT_FALSE(e.relay1.state);
}

TEST_F(ScheduleRule, ARestartingRuleTakesTheEdgeAtOnce) {
  adopt(
      R"({"name":"Restart","mode":"restart","triggers":[{"source":"schedule","windows":[{"from":"08:00","to":"08:01"}]}],
      "actions":[{"source":"switch","type":"follow","object_id":"relay_1"},{"source":"delay","delay_ms":120000}]})");
  boot_at(at(0, 8, 0));
  run_until(at(0, 8, 1));
  EXPECT_FALSE(e.relay1.state);
  EXPECT_EQ(engine.delays.size(), 1u);
}

TEST_F(ScheduleRule, TheConditionStillPicksTheBranch) {
  adopt(R"({"name":"Gated","triggers":[{"source":"schedule","windows":[{"from":"08:00","to":"20:00"}]}],
      "condition":{"type":"input","object_id":"in_1","state":"true"},
      "actions":[{"source":"switch","type":"follow","object_id":"relay_1"}],
      "else_actions":[{"source":"switch","type":"turn_on","object_id":"relay_2"}]})");
  boot_at(at(0, 10, 0));
  EXPECT_FALSE(e.relay1.state);
  EXPECT_TRUE(e.relay2.state);
}

}  // namespace esphome::automations::testing
