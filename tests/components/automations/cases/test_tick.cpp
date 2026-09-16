#include "common.h"

namespace esphome::automations::testing {

// 2026-02-02 10:00:00 UTC, an even second on a Monday.
static const time_t T0 = 1770026400;

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

TEST_F(Tick, ADisabledRuleStaysQuiet) {
  engine.rule(0)->set_enabled(false);
  engine.now = T0;
  engine.tick();
  EXPECT_EQ(e.relay1.writes, 0);
  EXPECT_EQ(e.relay2.writes, 1);
}

}  // namespace esphome::automations::testing
