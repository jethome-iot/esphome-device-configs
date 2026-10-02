#include "common.h"

namespace esphome::automations::testing {

// 2026-02-02 10:00:00 UTC, an even second on a Monday.
static const time_t T0 = 1770026400;

// One tick a second from `from` to `to`, both included: the clock running normally.
static void tick_through(FakeEngine &engine, time_t from, time_t to) {
  for (time_t t = from; t <= to; t++) {
    engine.now = t;
    engine.tick();
  }
}

class Tick : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_entities();
    engine.with_clock();
    auto every = build_rule(engine, R"({"name":"Every second","triggers":[{"source":"cron","cron":"* * * * * *"}],
        "actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})");
    auto even = build_rule(engine, R"({"name":"Even seconds","triggers":[{"source":"cron","cron":"*/2 * * * * *"}],
        "actions":[{"source":"switch","type":"toggle","object_id":"relay_2"}]})");
    ASSERT_NE(every, nullptr);
    ASSERT_NE(even, nullptr);
    engine.adopt(std::move(every));
    engine.adopt(std::move(even));
  }
  FakeEngine engine;
  Entities &e = entities();
};

TEST_F(Tick, WaitsForAValidTime) {
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 0);
  engine.now = T0;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_EQ(e.relay2.writes, 1);
}

TEST_F(Tick, FiresASecondOnce) {
  engine.now = T0;
  engine.tick();
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 1);
}

TEST_F(Tick, CatchesUpTheSecondsItMissed) {
  engine.now = T0;
  engine.tick();
  engine.now = T0 + 5;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 6);
  EXPECT_EQ(e.relay2.writes, 3);  // T0, T0+2, T0+4
}

TEST_F(Tick, AJumpAheadSkipsTheGap) {
  engine.now = T0;
  engine.tick();
  engine.now = T0 + 901;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 1);
  engine.now = T0 + 902;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(Tick, AShortStepBackWaitsForTheClockToPass) {
  engine.now = T0 + 10;
  engine.tick();
  engine.now = T0 + 5;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 1);
  engine.now = T0 + 11;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(Tick, ALongJumpBackStartsOver) {
  engine.now = T0 + 2000;
  engine.tick();
  engine.now = T0;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 2);
  engine.now = T0 + 1;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 3);
}

TEST_F(Tick, A900SecondJumpAheadIsCaughtUp) {
  engine.now = T0;
  engine.tick();
  engine.now = T0 + 900;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 901);
}

TEST_F(Tick, A900SecondStepBackStillWaits) {
  engine.now = T0 + 900;
  engine.tick();
  engine.now = T0;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 1);
  engine.now = T0 + 901;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(Tick, A901SecondJumpBackStartsOver) {
  engine.now = T0 + 901;
  engine.tick();
  engine.now = T0;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(Tick, ACatchUpAndAStepBackFireEachSecondOnce) {
  tick_through(engine, T0, T0 + 200);
  engine.now = T0 + 500;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 501);
  tick_through(engine, T0 + 201, T0 + 600);
  EXPECT_EQ(e.relay1.writes, 601);
}

TEST_F(Tick, ADisabledRuleStaysQuiet) {
  engine.rule(0)->set_enabled(false);
  engine.now = T0;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 0);
  EXPECT_EQ(e.relay2.writes, 1);
}

// A clock that ran ahead, past one daily moment, until a time sync pulled it back.
class TickAtAMoment : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_entities();
    engine.with_clock();
    auto rule = build_rule(engine, R"({"name":"At 10:05","triggers":[{"source":"cron","cron":"0 5 10 * * *"}],
        "actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})");
    ASSERT_NE(rule, nullptr);
    engine.adopt(std::move(rule));
  }
  static constexpr time_t AT = T0 + 300;
  FakeEngine engine;
  Entities &e = entities();
};

TEST_F(TickAtAMoment, AShortSyncBackDoesNotRepeatIt) {
  tick_through(engine, AT - 300, AT + 300);
  EXPECT_EQ(e.relay1.writes, 1);
  tick_through(engine, AT, AT + 600);
  EXPECT_EQ(e.relay1.writes, 1);
}

// The first run came at the wrong real time, so it runs again at the right one.
TEST_F(TickAtAMoment, ALongSyncBackRepeatsIt) {
  tick_through(engine, AT - 300, AT + 300);
  EXPECT_EQ(e.relay1.writes, 1);
  tick_through(engine, AT - 900, AT + 300);
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(TickAtAMoment, ALongJumpOverItAndBackFiresItOnce) {
  tick_through(engine, AT - 100, AT - 1);
  engine.now = AT + 1100;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 0);
  tick_through(engine, AT - 99, AT + 300);
  EXPECT_EQ(e.relay1.writes, 1);
}

}  // namespace esphome::automations::testing
