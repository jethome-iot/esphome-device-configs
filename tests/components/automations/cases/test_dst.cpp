#include "common.h"

namespace esphome::automations::testing {

// A change of the local offset at `at` (epoch seconds): the local clock is `before` seconds ahead
// of UTC until then, `after` seconds from then on.
struct Shift {
  time_t at;
  int32_t before;
  int32_t after;
};

static constexpr int32_t CET = 3600;
static constexpr int32_t CEST = 7200;
// Europe's 2026 switches: 2026-03-29 01:00:00 UTC, when 02:00 CET becomes 03:00 CEST, and
// 2026-10-25 01:00:00 UTC, when 03:00 CEST becomes 02:00 CET.
static constexpr Shift SPRING{1774746000, CET, CEST};
static constexpr Shift AUTUMN{1792890000, CEST, CET};
// America/Santiago switches at midnight: 2026-09-06 04:00:00 UTC, when Saturday 23:59:59 -04
// becomes Sunday 01:00:00 -03.
static constexpr Shift SANTIAGO{1788667200, -4 * 3600, -3 * 3600};
// Lord Howe Island moves its clock by half an hour: 2026-10-03 15:30:00 UTC, 02:00 +10:30 to
// 02:30 +11, and 2026-04-04 15:00:00 UTC, 02:00 +11 back to 01:30 +10:30.
static constexpr Shift LORD_HOWE_FORWARD{1791041400, 37800, 39600};
static constexpr Shift LORD_HOWE_BACK{1775314800, 39600, 37800};
// 2026-02-02 01:00:00 UTC: when the cases that change the zone itself change it.
static constexpr time_t MOVE = 1769994000;

// One tick at `t`, with the offset the clock has then.
static void tick_at(FakeEngine &engine, const Shift &shift, time_t t) {
  engine.now = t;
  engine.offset = t < shift.at ? shift.before : shift.after;
  engine.tick();
}

// One tick a second from `from` to `to`, both included, across `shift`.
static void tick_across(FakeEngine &engine, const Shift &shift, time_t from, time_t to) {
  for (time_t t = from; t <= to; t++)
    tick_at(engine, shift, t);
}

using Runs = std::vector<std::string>;

// Each relay notes the local time it was switched at, so a case says when its rule ran.
class Dst : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_entities();
    engine.with_clock();
    e.relay1.on_change = [this] { runs1.push_back(reading()); };
    e.relay2.on_change = [this] { runs2.push_back(reading()); };
  }
  void TearDown() override {
    e.relay1.on_change = nullptr;
    e.relay2.on_change = nullptr;
  }
  void add(const std::string &json) {
    auto rule = build_rule(engine, json.c_str());
    ASSERT_NE(rule, nullptr) << json;
    engine.adopt(std::move(rule));
  }
  // A rule that toggles `relay` on `cron`, and on `cron2` too when there is one.
  void add_toggle(const char *relay, const char *cron, const char *cron2 = nullptr) {
    std::string triggers = std::string(R"({"source":"cron","cron":")") + cron + R"("})";
    if (cron2 != nullptr)
      triggers += std::string(R"(,{"source":"cron","cron":")") + cron2 + R"("})";
    add(R"({"name":"Rule )" + std::to_string(++rules) + R"(","triggers":[)" + triggers +
        R"(],"actions":[{"source":"switch","type":"toggle","object_id":")" + relay + R"("}]})");
  }
  // The local clock as "02:30:00 CEST".
  std::string reading() const {
    const ESPTime t = ESPTime::from_epoch_utc(engine.now + engine.offset);
    const char *zone = engine.offset == CET ? "CET" : engine.offset == CEST ? "CEST" : engine.offset == 0 ? "UTC" : "?";
    char text[32];
    snprintf(text, sizeof(text), "%02d:%02d:%02d %s", t.hour, t.minute, t.second, zone);
    return text;
  }
  FakeEngine engine;
  Entities &e = entities();
  Runs runs1;
  Runs runs2;
  int rules{0};
};

// Spring: 02:00:00 to 02:59:59 never happen.

TEST_F(Dst, EveryHourRunsOnTheClockInSpring) {
  add_toggle("relay_1", "0 */15 * * * *");
  tick_across(engine, SPRING, SPRING.at - 1200, SPRING.at + 1200);
  EXPECT_EQ(runs1, (Runs{"01:45:00 CET", "03:00:00 CEST", "03:15:00 CEST"}));
}

// 03:00 above is the clock's own reading; here nothing reads :30 at the switch.
TEST_F(Dst, EveryHourDoesNotMakeUpTheMissingHour) {
  add_toggle("relay_1", "0 30 * * * *");
  tick_across(engine, SPRING, SPRING.at - 3600, SPRING.at + 3600);
  EXPECT_EQ(runs1, (Runs{"01:30:00 CET", "03:30:00 CEST"}));
}

// Every hour is what the field covers, not how it is written.
TEST_F(Dst, AllTwentyFourHoursListedIsEveryHour) {
  add_toggle("relay_1", "0 30 0-23 * * *");
  tick_across(engine, SPRING, SPRING.at - 3600, SPRING.at + 3600);
  EXPECT_EQ(runs1, (Runs{"01:30:00 CET", "03:30:00 CEST"}));
}

// Through to the next day's 02:30, which runs as usual.
TEST_F(Dst, ADailyRuleInTheMissingHourRunsAtTheSwitch) {
  add_toggle("relay_1", "0 30 2 * * *");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 86400 - 1800);
  EXPECT_EQ(runs1, (Runs{"03:00:00 CEST", "02:30:00 CEST"}));
}

// 03:00:00 is both the last skipped second and the clock's reading: one run.
TEST_F(Dst, ADailyRuleAtTheNewTimeRunsOnce) {
  add_toggle("relay_1", "0 0 3 * * *");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 3600);
  EXPECT_EQ(runs1, (Runs{"03:00:00 CEST"}));
}

// 02:00 is skipped, 03:00:00 is the clock's own reading: two runs, as 02:00 and 03:00 on any day.
TEST_F(Dst, AnHourRangeRunsForTheSkippedHourAndForThree) {
  add_toggle("relay_1", "0 0 1-6 * * *");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 600);
  EXPECT_EQ(runs1, (Runs{"03:00:00 CEST", "03:00:00 CEST"}));
}

TEST_F(Dst, ARuleTheMissingHourMatchesOftenRunsOnce) {
  add_toggle("relay_1", "0 */10 2 * * *");
  add_toggle("relay_2", "0 15 2 * * *", "0 45 2 * * *");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 3600);
  EXPECT_EQ(runs1, (Runs{"03:00:00 CEST"}));
  EXPECT_EQ(runs2, (Runs{"03:00:00 CEST"}));
}

// Both triggers match the same missing second: twice, as at 02:30 on any other day.
TEST_F(Dst, TwoTriggersOnOneMissingSecondRunAsOnAnyDay) {
  add_toggle("relay_1", "0 30 2 * * *", "0 30 2,14 * * *");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 600);
  EXPECT_EQ(e.relay1.writes, 2);
}

TEST_F(Dst, EveryTwoHoursMakesUpTheMissingTwo) {
  add_toggle("relay_1", "0 0 */2 * * *");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 3600);
  EXPECT_EQ(runs1, (Runs{"03:00:00 CEST", "04:00:00 CEST"}));
}

// The 02:40 rule is first in the list: the order is the times', so the relay ends off.
TEST_F(Dst, MissedRulesRunInTheOrderOfTheirTimes) {
  add(R"({"name":"Off at 02:40","triggers":[{"source":"cron","cron":"0 40 2 * * *"}],
      "actions":[{"source":"switch","type":"turn_off","object_id":"relay_1"}]})");
  add(R"({"name":"On at 02:10","triggers":[{"source":"cron","cron":"0 10 2 * * *"}],
      "actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 600);
  EXPECT_EQ(e.relay1.writes, 2);
  EXPECT_FALSE(e.relay1.state);
}

// A rule on file that did not build leaves its slot empty, ahead of one that did.
TEST_F(Dst, AnUnbuiltRuleIsPassedOverAtTheSwitch) {
  engine.adopt(nullptr);
  add_toggle("relay_1", "0 30 2 * * *");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 600);
  EXPECT_EQ(runs1, (Runs{"03:00:00 CEST"}));
}

TEST_F(Dst, ADisabledRuleStaysQuietAtTheSwitch) {
  add_toggle("relay_1", "0 30 2 * * *");
  engine.rule(0)->set_enabled(false);
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 600);
  EXPECT_EQ(e.relay1.writes, 0);
}

// The second before the switch is not part of the missing hour, nor is the one after 03:00:00.
TEST_F(Dst, TheSecondsAroundTheMissingHourRunOnTime) {
  add_toggle("relay_1", "59 59 1 * * *");
  add_toggle("relay_2", "1 0 3 * * *");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 600);
  EXPECT_EQ(runs1, (Runs{"01:59:59 CET"}));
  EXPECT_EQ(runs2, (Runs{"03:00:01 CEST"}));
}

// The input trigger next to the cron one is no time to match.
TEST_F(Dst, ARuleWithAnInputTriggerTooRunsAtTheSwitch) {
  add(R"({"name":"Press or 02:30","triggers":[{"source":"input","type":"state_change","object_id":"in_1"},
      {"source":"cron","cron":"0 30 2 * * *"}],"actions":[{"source":"switch","type":"toggle","object_id":"relay_1"}]})");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 600);
  EXPECT_EQ(runs1, (Runs{"03:00:00 CEST"}));
}

TEST_F(Dst, ARuleWithBothKindsOfTriggerInSpring) {
  add_toggle("relay_1", "0 10 * * * *", "0 30 2 * * *");
  tick_across(engine, SPRING, SPRING.at - 3600, SPRING.at + 1200);
  EXPECT_EQ(runs1, (Runs{"01:10:00 CET", "03:00:00 CEST", "03:10:00 CEST"}));
}

// A loop stalled from 01:56:40 to 03:01:40: the missing hour still runs once, and 01:58, a real
// second of the stall, is caught up as on any other night.
TEST_F(Dst, AStallOverTheSpringSwitch) {
  add_toggle("relay_1", "0 30 2 * * *");
  add_toggle("relay_2", "0 58 * * * *");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at - 200);
  tick_at(engine, SPRING, SPRING.at + 100);
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_EQ(e.relay2.writes, 1);
  tick_across(engine, SPRING, SPRING.at + 101, SPRING.at + 3000);
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_EQ(e.relay2.writes, 1);
}

TEST_F(Dst, A900SecondStallIsStillASwitch) {
  add_toggle("relay_1", "0 30 2 * * *");
  tick_at(engine, SPRING, SPRING.at - 500);
  tick_at(engine, SPRING, SPRING.at + 400);
  EXPECT_EQ(e.relay1.writes, 1);
}

// Past 900 s it is a jump ahead, whose gap is skipped.
TEST_F(Dst, A901SecondJumpOverTheSwitchSkipsIt) {
  add_toggle("relay_1", "0 30 2 * * *");
  tick_at(engine, SPRING, SPRING.at - 500);
  tick_at(engine, SPRING, SPRING.at + 401);
  EXPECT_EQ(e.relay1.writes, 0);
}

// Autumn: 02:00:00 to 02:59:59 happen twice, in CEST and then in CET.

TEST_F(Dst, EveryHourRunsThroughTheRepeatedHour) {
  add_toggle("relay_1", "0 */15 * * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 3599);
  EXPECT_EQ(runs1, (Runs{"02:00:00 CEST", "02:15:00 CEST", "02:30:00 CEST", "02:45:00 CEST", "02:00:00 CET",
                         "02:15:00 CET", "02:30:00 CET", "02:45:00 CET"}));
}

// Through to the next day's 02:30, which runs as usual.
TEST_F(Dst, ADailyRuleInTheRepeatedHourRunsOnce) {
  add_toggle("relay_1", "0 30 2 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 86400 + 1800);
  EXPECT_EQ(runs1, (Runs{"02:30:00 CEST", "02:30:00 CET"}));
}

TEST_F(Dst, ADailyRuleAfterTheRepeatedHourRunsOnce) {
  add_toggle("relay_1", "0 0 3 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 3700);
  EXPECT_EQ(runs1, (Runs{"03:00:00 CET"}));
}

// The switch itself reads 02:00:00, an hour after 02:00 CEST ran.
TEST_F(Dst, NothingAtHoursRunsOnTheAutumnSwitch) {
  add_toggle("relay_1", "0 0 2 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3610, AUTUMN.at);
  EXPECT_EQ(runs1, (Runs{"02:00:00 CEST"}));
  tick_across(engine, AUTUMN, AUTUMN.at + 1, AUTUMN.at + 3600);
  EXPECT_EQ(runs1, (Runs{"02:00:00 CEST"}));
}

// 02:59:59 CET reads what the clock read last before the switch: not past it yet.
TEST_F(Dst, TheHoldEndsOnceTheClockPassesItsLastReading) {
  add_toggle("relay_1", "59 59 2 * * *");
  add_toggle("relay_2", "0 0 3 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 10, AUTUMN.at + 3610);
  EXPECT_EQ(runs1, (Runs{"02:59:59 CEST"}));
  EXPECT_EQ(runs2, (Runs{"03:00:00 CET"}));
}

TEST_F(Dst, ARuleWithBothKindsOfTriggerInAutumn) {
  add_toggle("relay_1", "0 10 * * * *", "0 30 2 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 3600);
  EXPECT_EQ(runs1, (Runs{"02:10:00 CEST", "02:30:00 CEST", "02:10:00 CET"}));
}

// A loop stalled from 02:56:40 CEST to 02:01:40 CET. Every hour keeps the stall's real 02:58
// CEST and the 02:58 CET after it; 02:30 ran before the stall and stays held.
TEST_F(Dst, AStallOverTheAutumnSwitch) {
  add_toggle("relay_1", "0 30 2 * * *");
  add_toggle("relay_2", "0 58 * * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at - 200);
  tick_at(engine, AUTUMN, AUTUMN.at + 100);
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_EQ(e.relay2.writes, 1);
  tick_across(engine, AUTUMN, AUTUMN.at + 101, AUTUMN.at + 3600);
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_EQ(e.relay2.writes, 2);
}

// The hold lasts past 02:56:40, the clock's last reading before the stall, so 02:58, which the
// stall swallowed in CEST, runs in CET.
TEST_F(Dst, AnAtHoursTimeTheAutumnStallSwallowedRunsInTheRepeat) {
  add_toggle("relay_1", "0 58 2 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 600, AUTUMN.at - 200);
  tick_at(engine, AUTUMN, AUTUMN.at + 100);
  EXPECT_EQ(e.relay1.writes, 0);
  tick_across(engine, AUTUMN, AUTUMN.at + 101, AUTUMN.at + 3600);
  EXPECT_EQ(runs1, (Runs{"02:58:00 CET"}));
}

// A sync that pulls the clock back over 15 minutes in the held hour ends the hold: what it
// passes again runs again, as after any long jump back.
TEST_F(Dst, ALongJumpBackEndsTheHold) {
  add_toggle("relay_1", "0 30 2 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 1500);
  tick_across(engine, AUTUMN, AUTUMN.at + 500, AUTUMN.at + 2400);
  EXPECT_EQ(runs1, (Runs{"02:30:00 CEST", "02:30:00 CET"}));
}

TEST_F(Dst, AShortStepBackKeepsTheHold) {
  add_toggle("relay_1", "0 30 2 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 1500);
  tick_across(engine, AUTUMN, AUTUMN.at + 800, AUTUMN.at + 2400);
  EXPECT_EQ(runs1, (Runs{"02:30:00 CEST"}));
}

// From 02:10 CET to 02:25:01 CET: still short of 02:59:59, so 02:45 is still held.
TEST_F(Dst, AJumpAheadInTheHeldHourKeepsTheHold) {
  add_toggle("relay_1", "0 45 2 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 600);
  tick_across(engine, AUTUMN, AUTUMN.at + 1501, AUTUMN.at + 3600);
  EXPECT_EQ(runs1, (Runs{"02:45:00 CEST"}));
}

// A restart at 02:20 CET: the record of the switch keeps 02:30 from running a second time,
// and 03:00 runs once the clock is past the hour.
TEST_F(Dst, ARestartInTheRepeatedHourKeepsTheHold) {
  add_toggle("relay_1", "0 30 2 * * *");
  add_toggle("relay_2", "0 0 3 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 1200);
  engine.restart();
  tick_across(engine, AUTUMN, AUTUMN.at + 1260, AUTUMN.at + 3700);
  EXPECT_EQ(runs1, (Runs{"02:30:00 CEST"}));
  EXPECT_EQ(runs2, (Runs{"03:00:00 CET"}));
}

// A restart in spring has nothing to read back: the skipped hour ran at the switch.
TEST_F(Dst, ARestartAfterTheSpringSwitchHoldsNothing) {
  add_toggle("relay_1", "0 30 3 * * *");
  tick_across(engine, SPRING, SPRING.at - 600, SPRING.at + 600);
  engine.restart();
  tick_across(engine, SPRING, SPRING.at + 660, SPRING.at + 2000);
  EXPECT_EQ(runs1, (Runs{"03:30:00 CEST"}));
}

// The record is for that night: a restart at the same hour a day later holds nothing.
TEST_F(Dst, ARecordOfTheSwitchHoldsNothingTheNextNight) {
  add_toggle("relay_1", "0 30 2 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 1200);
  engine.restart();
  tick_across(engine, AUTUMN, AUTUMN.at + 86400 + 1200, AUTUMN.at + 86400 + 1900);
  EXPECT_EQ(runs1, (Runs{"02:30:00 CEST", "02:30:00 CET"}));
}

// Back at 02:30 UTC, in a zone picked while it was off: its clock reads the repeated hour
// again, but the hour is over by the real time, so nothing is held.
TEST_F(Dst, ARecordOfTheSwitchEndsWithItsHourInAnyZone) {
  add_toggle("relay_1", "0 35 2 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 1200);
  engine.restart();
  tick_across(engine, Shift{0, 0, 0}, AUTUMN.at + 5400, AUTUMN.at + 6000);
  EXPECT_EQ(runs1, (Runs{"02:35:00 CEST", "02:35:00 UTC"}));
}

// A sync that pulls the clock back over 15 minutes ends the hold, and a restart after it does
// not bring the hold back.
TEST_F(Dst, ARestartAfterAJumpBackHoldsNothing) {
  add_toggle("relay_1", "0 30 2 * * *");
  tick_across(engine, AUTUMN, AUTUMN.at - 3600, AUTUMN.at + 1200);
  tick_at(engine, AUTUMN, AUTUMN.at + 200);
  engine.restart();
  tick_across(engine, AUTUMN, AUTUMN.at + 201, AUTUMN.at + 1900);
  EXPECT_EQ(runs1, (Runs{"02:30:00 CEST", "02:30:00 CET"}));
}

// Other zones and other offsets.

// The missing hour is Sunday's 00:00 to 00:59:59, not Saturday's.
TEST_F(Dst, AMidnightSwitchKeepsTheDayOfTheMissingHour) {
  add_toggle("relay_1", "0 30 0 * * 1");
  add_toggle("relay_2", "0 30 0 * * 7");
  tick_across(engine, SANTIAGO, SANTIAGO.at - 600, SANTIAGO.at);
  EXPECT_EQ(e.relay1.writes, 1);
  tick_across(engine, SANTIAGO, SANTIAGO.at + 1, SANTIAGO.at + 3600);
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_EQ(e.relay2.writes, 0);
}

TEST_F(Dst, AHalfHourSwitchForwardMakesUpTheMissingTime) {
  add_toggle("relay_1", "0 15 2 * * *");
  tick_across(engine, LORD_HOWE_FORWARD, LORD_HOWE_FORWARD.at - 600, LORD_HOWE_FORWARD.at);
  EXPECT_EQ(e.relay1.writes, 1);
  tick_across(engine, LORD_HOWE_FORWARD, LORD_HOWE_FORWARD.at + 1, LORD_HOWE_FORWARD.at + 3600);
  EXPECT_EQ(e.relay1.writes, 1);
}

TEST_F(Dst, AHalfHourSwitchBackHoldsTheRepeatedTime) {
  add_toggle("relay_1", "0 45 1 * * *");
  tick_across(engine, LORD_HOWE_BACK, LORD_HOWE_BACK.at - 1800, LORD_HOWE_BACK.at + 3600);
  EXPECT_EQ(e.relay1.writes, 1);
}

// A zone an hour west picked at 14:20 CET and taken back two minutes later: 13:30 ran already
// and does not run again, while 14:21, which neither zone's clock read, runs at the undo.
TEST_F(Dst, AZoneTakenBackRunsNothingTwice) {
  add_toggle("relay_1", "0 30 13 * * *");
  add_toggle("relay_2", "0 21 14 * * *");
  const time_t away = MOVE + 12 * 3600 + 20 * 60;
  for (time_t t = away - 3600; t <= away + 600; t++) {
    engine.now = t;
    engine.offset = t >= away && t < away + 120 ? 0 : CET;
    engine.tick();
  }
  EXPECT_EQ(runs1, (Runs{"13:30:00 CET"}));
  EXPECT_EQ(runs2, (Runs{"14:22:00 CET"}));
}

// Two hours is a new zone, not a DST switch: the times in between are lost, as in a jump ahead.
TEST_F(Dst, ANewZoneAheadSkipsTheTimesBetween) {
  add_toggle("relay_1", "0 30 2 * * *");
  tick_across(engine, Shift{MOVE, 0, 7200}, MOVE - 600, MOVE + 1800);
  EXPECT_EQ(e.relay1.writes, 0);
}

TEST_F(Dst, ANewZoneBehindRepeatsTheTimesBetween) {
  add_toggle("relay_1", "0 30 1 * * *");
  tick_across(engine, Shift{MOVE, 7200, 0}, MOVE - 6000, MOVE + 2400);
  EXPECT_EQ(e.relay1.writes, 2);
}

// A quarter of an hour is too small for a DST switch.
TEST_F(Dst, AQuarterHourAheadSkipsTheTimesBetween) {
  add_toggle("relay_1", "0 10 1 * * *");
  tick_across(engine, Shift{MOVE, 0, 900}, MOVE - 600, MOVE + 1800);
  EXPECT_EQ(e.relay1.writes, 0);
}

TEST_F(Dst, AQuarterHourBehindRepeatsTheTimesBetween) {
  add_toggle("relay_1", "0 5 1 * * *");
  tick_across(engine, Shift{MOVE, 900, 0}, MOVE - 1200, MOVE + 1200);
  EXPECT_EQ(e.relay1.writes, 2);
}

}  // namespace esphome::automations::testing
