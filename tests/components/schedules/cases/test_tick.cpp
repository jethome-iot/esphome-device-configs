// When a mode's list runs: the first valid clock, each switch point once, a hand change kept
// until the next point, and what a set clock, a new zone, DST or a stalled loop does. Every
// mode here is one action; lists of several, with waits, are test_lists.cpp's.
#include "common.h"

namespace esphome::schedules::testing {

class Tick : public EngineTest {
 protected:
  // `config` created on an engine whose clock is not valid yet; the first valid tick reads `when`.
  void start(int64_t when, const ScheduleConfig &config = daily()) {
    this->boot();
    this->create(config);
    this->engine->local = when;
    this->engine->tick();
  }
  std::vector<bool> &writes() { return this->e.relay1.writes; }
};

TEST_F(Tick, NothingMovesBeforeTheClockIsValid) {
  this->boot();
  this->create(daily());
  for (int i = 0; i < 5; i++)
    this->engine->tick();
  EXPECT_TRUE(this->writes().empty());
  const ScheduleStatus status = this->engine->status("porch");
  EXPECT_EQ(ScheduleState::WAITING_FOR_CLOCK, status.state);
  EXPECT_EQ("Waiting for the clock", status.reason);
  PointRef now;
  PointRef next;
  EXPECT_FALSE(this->engine->now_and_next(*this->engine->get("porch"), &now, &next));
  std::string day;
  std::string time;
  EXPECT_FALSE(this->engine->clock_text(&day, &time));

  this->engine->local = at_local(0, 9, 0);
  this->engine->tick();
  EXPECT_EQ(std::vector<bool>{true}, this->writes()) << "the first valid tick runs the mode in force";
  EXPECT_EQ(ScheduleState::RUNNING, this->state("porch"));
}

TEST_F(Tick, ABootInsideAStretchAppliesItOnce) {
  this->boot();
  this->create(daily());
  this->engine->local = at_local(2, 12, 0);
  this->reboot();
  EXPECT_EQ(std::vector<bool>{true}, this->writes());
  this->engine->run(180);
  EXPECT_EQ(std::vector<bool>{true}, this->writes()) << "the stretch it is in does not apply again";
}

TEST_F(Tick, EachSwitchPointAppliesOnce) {
  this->start(at_local(0, 7, 58, 30));
  EXPECT_EQ(std::vector<bool>{false}, this->writes()) << "Sunday's 20:00 holds until Monday's 08:00";
  this->engine->run(120);
  EXPECT_EQ((std::vector<bool>{false, true}), this->writes());
  this->engine->run(3600);
  EXPECT_EQ((std::vector<bool>{false, true}), this->writes());
}

TEST_F(Tick, APointWithTheSameValueAppliesAgain) {
  this->start(at_local(0, 8, 30), relay_doc("Porch", "relay_1", {on_at(wm(0, 8, 0)), on_at(wm(0, 9, 0))}));
  EXPECT_EQ(std::vector<bool>{true}, this->writes());
  this->e.relay1.turn_off();
  this->engine->run(31 * 60);
  EXPECT_EQ((std::vector<bool>{true, false, true}), this->writes());
  EXPECT_TRUE(this->e.relay1.state);
}

TEST_F(Tick, AHandChangeHoldsUntilTheNextPoint) {
  this->start(at_local(0, 9, 0));
  this->e.relay1.turn_off();
  this->engine->run(11 * 3600 - 1);
  EXPECT_EQ((std::vector<bool>{true, false}), this->writes()) << "nothing until 20:00";
  this->engine->run(1);
  EXPECT_EQ((std::vector<bool>{true, false, false}), this->writes()) << "20:00 applies, though the relay is off";
}

TEST_F(Tick, AJumpForwardRunsTheModeInForce) {
  this->start(at_local(0, 7, 0));
  this->engine->jump_to(at_local(0, 9, 0));
  EXPECT_EQ((std::vector<bool>{false, true}), this->writes());
  EXPECT_TRUE(LogCapture::instance().warned("Clock moved by +7199 s"));
}

TEST_F(Tick, AJumpBackRunsTheModeInForceAndThePointsComeAgain) {
  this->start(at_local(0, 9, 0));
  this->engine->jump_to(at_local(0, 7, 0));
  EXPECT_EQ((std::vector<bool>{true, false}), this->writes());
  EXPECT_TRUE(LogCapture::instance().warned("Clock moved by -7201 s"));
  this->engine->run(3600);
  EXPECT_EQ((std::vector<bool>{true, false, true}), this->writes()) << "08:00 again";
}

TEST_F(Tick, AStepBackOfUnderAMinuteNeverAppliesAPointAgain) {
  this->start(at_local(0, 7, 59, 50));
  this->engine->run(15);
  EXPECT_EQ((std::vector<bool>{false, true}), this->writes());
  this->e.relay1.turn_off();
  // SNTP pulls the clock back 40 s, behind 08:00.
  this->engine->jump_to(this->engine->local - 40);
  this->engine->run(90);
  EXPECT_EQ((std::vector<bool>{false, true, false}), this->writes()) << "08:00 was applied already";
  EXPECT_FALSE(LogCapture::instance().warned("Clock moved"));
}

TEST_F(Tick, AStepForwardOfUnderAMinuteAppliesThePointItCrosses) {
  this->start(at_local(0, 7, 59, 30));
  this->engine->jump_to(at_local(0, 8, 0, 15));
  EXPECT_EQ((std::vector<bool>{false, true}), this->writes());
  EXPECT_FALSE(LogCapture::instance().warned("Clock moved"));
}

TEST_F(Tick, AStalledLoopAppliesOnlyTheLatestPointItMissed) {
  this->start(at_local(0, 7, 59),
              relay_doc("Porch", "relay_1",
                        {on_at(wm(0, 8, 0)), off_at(wm(0, 8, 2)), on_at(wm(0, 8, 4)), off_at(wm(6, 22, 0))}));
  this->engine->stall(360);
  EXPECT_EQ((std::vector<bool>{false, true}), this->writes()) << "08:04 alone";
  EXPECT_FALSE(LogCapture::instance().warned("Clock moved"));
}

// Two points at one minute: what the first one's relay does may stop the second schedule.
TEST_F(Tick, ASchedulePointStoppedByAnEarlierOneInTheSameTickStaysStopped) {
  this->boot();
  this->create(relay_doc("A", "relay_1", {on_at(wm(0, 8, 0)), off_at(wm(6, 22, 0))}));
  this->create(relay_doc("B", "relay_2", {on_at(wm(0, 8, 0)), off_at(wm(6, 22, 0))}));
  this->engine->local = at_local(0, 7, 59, 30);
  this->engine->tick();
  this->e.relay1.on_write = [this]() { this->engine->set_enabled("b", false); };
  this->engine->run(60);
  EXPECT_TRUE(this->e.relay1.state);
  EXPECT_EQ(std::vector<bool>{false}, this->e.relay2.writes) << "the first tick's off, and nothing at 08:00";
}

TEST_F(Tick, ANewTimeZoneIsAJump) {
  this->start(at_local(0, 10, 0));
  this->engine->jump_to(at_local(0, 13, 0));
  EXPECT_EQ((std::vector<bool>{true, true}), this->writes()) << "the value in force, again";
  EXPECT_TRUE(LogCapture::instance().warned("Clock moved by +10799 s"));
}

// Clocks go from 01:59:59 to 03:00:00: what holds at 03:00 applies, 02:30's value.
TEST_F(Tick, SpringForwardAppliesWhatHoldsAfterTheGap) {
  this->start(at_local(6, 1, 59, 59),
              relay_doc("Porch", "relay_1", {off_at(wm(6, 1, 0)), on_at(wm(6, 2, 30)), off_at(wm(6, 5, 0))}));
  EXPECT_EQ(std::vector<bool>{false}, this->writes());
  this->engine->jump_to(at_local(6, 3, 0));
  EXPECT_EQ((std::vector<bool>{false, true}), this->writes());
}

// Clocks go from 02:59:59 back to 02:00:00: the points in the repeated hour apply twice.
TEST_F(Tick, FallBackPlaysTheRepeatedHourTwice) {
  this->start(at_local(6, 2, 59, 59),
              relay_doc("Porch", "relay_1", {off_at(wm(6, 1, 0)), on_at(wm(6, 2, 30)), off_at(wm(6, 5, 0))}));
  EXPECT_EQ(std::vector<bool>{true}, this->writes());
  this->engine->jump_to(at_local(6, 2, 0));
  EXPECT_EQ((std::vector<bool>{true, false}), this->writes()) << "01:00's value holds at 02:00";
  this->engine->run(31 * 60);
  EXPECT_EQ((std::vector<bool>{true, false, true}), this->writes()) << "02:30 once more";
}

TEST_F(Tick, SundayNightRunsIntoMonday) {
  this->start(at_local(6, 23, 59, 30), relay_doc("Porch", "relay_1", {on_at(wm(0, 0, 0)), off_at(wm(6, 23, 0))}));
  EXPECT_EQ(std::vector<bool>{false}, this->writes());
  this->engine->run(60);
  EXPECT_EQ((std::vector<bool>{false, true}), this->writes());
}

TEST_F(Tick, TheWeeksTurnIsNoPoint) {
  this->start(at_local(6, 23, 59, 30), relay_doc("Porch", "relay_1", {on_at(wm(2, 10, 0))}));
  this->engine->run(120);
  EXPECT_EQ(std::vector<bool>{true}, this->writes());
}

TEST_F(Tick, NowAndNextFollowTheClock) {
  this->start(at_local(0, 9, 0));
  const ScheduleConfig &config = *this->engine->get("porch");
  PointRef now;
  PointRef next;
  ASSERT_TRUE(this->engine->now_and_next(config, &now, &next));
  EXPECT_EQ(0, now.day);
  EXPECT_EQ(8 * 60, now.minute);
  EXPECT_EQ("on", config.modes[now.point->mode].id);
  EXPECT_EQ(0, next.day);
  EXPECT_EQ(20 * 60, next.minute);
  EXPECT_EQ("off", config.modes[next.point->mode].id);

  this->engine->jump_to(at_local(6, 21, 0));
  ASSERT_TRUE(this->engine->now_and_next(config, &now, &next));
  EXPECT_EQ(6, now.day);
  EXPECT_EQ(20 * 60, now.minute);
  EXPECT_EQ(0, next.day) << "next week's Monday";
  EXPECT_EQ(8 * 60, next.minute);
}

TEST_F(Tick, NowAndNextAreThereForADisabledScheduleToo) {
  this->start(at_local(0, 9, 0), daily("Porch", "relay_1", false));
  EXPECT_TRUE(this->writes().empty());
  PointRef now;
  PointRef next;
  EXPECT_TRUE(this->engine->now_and_next(*this->engine->get("porch"), &now, &next));
  EXPECT_EQ(ScheduleState::DISABLED, this->state("porch"));
}

TEST_F(Tick, TheClockTextIsTheDevicesLocalTime) {
  this->boot(at_local(2, 10, 15, 42));
  std::string day;
  std::string time;
  ASSERT_TRUE(this->engine->clock_text(&day, &time));
  EXPECT_EQ("wed", day);
  EXPECT_EQ("10:15:42", time);
}

TEST_F(Tick, EnablingRunsTheModeInForceOnce) {
  this->start(at_local(0, 9, 0), daily("Porch", "relay_1", false));
  Result enabled = this->engine->set_enabled("porch", true);
  ASSERT_TRUE(enabled.ok) << enabled.error;
  EXPECT_EQ(std::vector<bool>{true}, this->writes());
  ASSERT_TRUE(this->engine->set_enabled("porch", true).ok);
  EXPECT_EQ(std::vector<bool>{true}, this->writes()) << "on to on applies nothing";
}

// Every Save of an enabled schedule runs the mode in force again, a rename too.
TEST_F(Tick, ASaveOfAnEnabledScheduleRunsTheModeInForce) {
  this->start(at_local(0, 9, 0));
  this->e.relay1.turn_off();
  Result saved = this->engine->update("porch", daily("Porch light"));
  ASSERT_TRUE(saved.ok) << saved.error;
  EXPECT_EQ((std::vector<bool>{true, false, true}), this->writes());
  ASSERT_TRUE(this->engine->update("porch", daily("Porch light", "relay_1", false)).ok);
  EXPECT_EQ((std::vector<bool>{true, false, true}), this->writes()) << "a disabled one applies nothing";
}

TEST_F(Tick, DisablingOrDeletingLeavesTheTargetAsItIs) {
  this->start(at_local(0, 9, 0));
  ASSERT_TRUE(this->engine->set_enabled("porch", false).ok);
  this->engine->run(12 * 3600);
  EXPECT_EQ(std::vector<bool>{true}, this->writes());
  EXPECT_TRUE(this->e.relay1.state);
  ASSERT_TRUE(this->engine->set_enabled("porch", true).ok);
  ASSERT_TRUE(this->engine->remove("porch").ok);
  EXPECT_EQ((std::vector<bool>{true, false}), this->writes()) << "the enable applied 21:00's off, the delete nothing";
}

}  // namespace esphome::schedules::testing
