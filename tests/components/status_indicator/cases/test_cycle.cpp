#include "common.h"

namespace esphome::status_indicator::testing {

using State = IndicatorState;
using Levels = std::vector<bool>;
using Lines = std::vector<std::string>;

// A blinks twice with the blink_n times, B once, longer, with a shorter pause; the round pause is
// neither pause, so a gap names the pause it is.
constexpr uint8_t A_COUNT = 2;
constexpr uint32_t B_ON = 350;
constexpr uint32_t B_PAUSE = 400;
constexpr uint32_t ROUND = 900;

class CycleTest : public StatusIndicatorTest {
 protected:
  void SetUp() override {
    StatusIndicatorTest::SetUp();
    this->led->set_cycle(ROUND);
  }

  void add_a(FakeCondition *holds) { this->led->add_signal(holds, State::BLINK_N, A_COUNT, N_ON, N_OFF, N_PAUSE); }
  void add_b(FakeCondition *holds) { this->led->add_signal(holds, State::BLINK_N, 1, B_ON, N_OFF, B_PAUSE); }
};

TEST_F(CycleTest, NoneHoldingAtFirstLeavesTheLedAlone) {
  this->add_a(condition());
  this->add_b(condition());
  this->led->blink_slow();
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), -1);
  EXPECT_EQ(this->led->get_state(), State::BLINK_SLOW);
}

// A's own pause hands over to B, the last, whose series ends the round on ROUND; then A again.
TEST_F(CycleTest, TwoCountsTakeTurnsAndTheLastEndsTheRound) {
  this->add_a(condition(true));
  this->add_b(condition(true));
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 0);
  EXPECT_EQ(this->led->get_state(), State::BLINK_N);
  expect_writes({true, false, true, false, true, false, true, false, true, false, true},
                {N_ON, N_OFF, N_ON, N_PAUSE, B_ON, ROUND, N_ON, N_OFF, N_ON, N_PAUSE});
  EXPECT_EQ(this->led->get_signal(), 1);
}

// The last in the order, wrapping round to itself: no round, as without a cycle.
TEST_F(CycleTest, ACountHoldingAloneKeepsItsOwnPause) {
  this->add_a(condition());
  this->add_b(condition(true));
  this->led->update();
  expect_writes({true, false, true, false, true}, {B_ON, B_PAUSE, B_ON, B_PAUSE});
  EXPECT_EQ(this->led->get_signal(), 1);
}

TEST_F(CycleTest, NoRoundPauseKeepsTheLastCountsOwn) {
  this->led->set_cycle(0);
  this->add_a(condition(true));
  this->add_b(condition(true));
  this->led->update();
  expect_writes({true, false, true, false, true, false, true}, {N_ON, N_OFF, N_ON, N_PAUSE, B_ON, B_PAUSE});
}

// Earlier in the order than B, A would win at once without a cycle; here B's series runs out and
// ends the round, now that two hold.
TEST_F(CycleTest, ACountThatComesToHoldWaitsForItsTurn) {
  FakeCondition *a = condition();
  this->add_a(a);
  this->add_b(condition(true));
  this->led->update();
  ASSERT_EQ(this->led->get_signal(), 1);
  a->holds = true;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 1);
  expect_writes({true, false, true, false, true, false, true}, {B_ON, ROUND, N_ON, N_OFF, N_ON, N_PAUSE});
}

// In the dark between A's two blinks: B lights at once, not when A's series would have ended.
TEST_F(CycleTest, TheCountShownThatStopsHandsOverAtOnce) {
  FakeCondition *a = condition(true);
  this->add_a(a);
  this->add_b(condition(true));
  this->led->update();
  run_for(N_ON + N_OFF / 2);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 2);
  a->holds = false;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 1);
  EXPECT_EQ(this->pin->writes.size(), 3u);
  // Then B alone, on its own pause.
  run_until_writes(5);
  ASSERT_EQ(this->pin->levels(5), Levels({true, false, true, false, true}));
  EXPECT_GE(this->pin->gap(3), B_ON);
  EXPECT_LE(this->pin->gap(3), B_ON + this->late());
  EXPECT_GE(this->pin->gap(4), B_PAUSE);
  EXPECT_LE(this->pin->gap(4), B_PAUSE + this->late());
}

// The next that holds after the one that stopped, not the first, and round past the end.
TEST_F(CycleTest, AHandoverGoesToTheNextCountAfterTheOneThatStopped) {
  FakeCondition *a = condition();
  FakeCondition *b = condition(true);
  FakeCondition *c = condition(true);
  this->add_a(a);
  this->add_b(b);
  this->led->add_signal(c, State::BLINK_N, 3, N_ON, N_OFF, N_PAUSE);
  this->led->update();
  ASSERT_EQ(this->led->get_signal(), 1);

  a->holds = true;
  b->holds = false;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 2);

  b->holds = true;
  c->holds = false;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 0);
}

// Nothing may fire into the series once no count holds.
TEST_F(CycleTest, TheLastCountThatStopsTurnsItOff) {
  FakeCondition *a = condition(true);
  this->add_a(a);
  this->add_b(condition());
  this->led->update();
  a->holds = false;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), -1);
  EXPECT_EQ(this->led->get_state(), State::OFF);
  // Past where A would have written its next off and on.
  run_for(N_ON + N_OFF + N_ON / 2);
  EXPECT_EQ(this->pin->levels(), Levels({true, false}));
}

// B had the turn when the state came; after it the turns start from the first count again.
TEST_F(CycleTest, AStateSignalWinsOutrightAndTheTurnsStartOverAfterIt) {
  FakeCondition *on = condition();
  FakeCondition *a = condition();
  this->led->add_signal(on, State::ON);
  this->add_a(a);
  this->add_b(condition(true));
  this->led->update();
  ASSERT_EQ(this->led->get_signal(), 2);

  a->holds = true;
  on->holds = true;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 0);
  EXPECT_EQ(this->led->get_state(), State::ON);
  // Lit by B already, and past where B's series would have written its off.
  run_for(B_ON + N_OFF);
  EXPECT_EQ(this->pin->levels(), Levels({true}));

  on->holds = false;
  const uint32_t resumed = millis();
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 1);
  EXPECT_EQ(this->led->get_state(), State::BLINK_N);
  // Still lit, so A's first write is its off; then A on its own pause to B.
  this->pin->writes.clear();
  expect_writes({false, true, false, true}, {N_OFF, N_ON, N_PAUSE});
  EXPECT_GE(this->pin->writes[0].at - resumed, N_ON);
  EXPECT_LE(this->pin->writes[0].at - resumed, N_ON + this->late());
  EXPECT_EQ(this->led->get_signal(), 2);
}

// Past a state that holds nothing else can show, so nothing else is asked. Below one, every
// signal is, as each count that holds takes a turn.
TEST_F(CycleTest, NoConditionAfterAHoldingStateIsChecked) {
  FakeCondition *slow = condition();
  FakeCondition *fast = condition();
  FakeCondition *a = condition(true);
  FakeCondition *b = condition(true);
  this->led->add_signal(slow, State::BLINK_SLOW);
  this->led->add_signal(fast, State::BLINK_FAST);
  this->add_a(a);
  this->add_b(b);
  this->led->update();
  EXPECT_EQ(slow->checks, 1);
  EXPECT_EQ(fast->checks, 1);
  EXPECT_EQ(a->checks, 1);
  EXPECT_EQ(b->checks, 1);

  fast->holds = true;
  CapturedLog &log = CapturedLog::instance();
  log.clear();
  this->led->update();
  this->led->update();
  EXPECT_EQ(slow->checks, 3);
  EXPECT_EQ(fast->checks, 3);
  EXPECT_EQ(a->checks, 1);
  EXPECT_EQ(b->checks, 1);
  EXPECT_EQ(this->led->get_signal(), 1);
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);
  // Unasked, the counts are not among those that hold.
  EXPECT_EQ(log.debug(), Lines({"Signals: 1", "State: BLINK_FAST"}));
}

// While A holds, the action stays; BLINK_N after it repeats A's series, which hands over no more.
TEST_F(CycleTest, AnActionStopsTheTurns) {
  this->add_a(condition(true));
  this->add_b(condition(true));
  this->led->update();
  this->led->turn_on();
  this->led->update();
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 0);
  EXPECT_EQ(this->led->get_state(), State::ON);

  this->led->set_state(State::BLINK_N);
  // Lit throughout, so the first write after the first update's is A's off.
  expect_writes({true, false, true, false, true, false, true, false}, {N_ON, N_OFF, N_ON, N_PAUSE, N_ON, N_OFF, N_ON});
  EXPECT_EQ(this->led->get_signal(), 0);
}

// The action's own series repeats, on its own pause, in place of A's turn.
TEST_F(CycleTest, ABlinkNActionRepeatsItsOwnSeries) {
  const uint32_t on = 600;
  const uint32_t pause = 500;
  this->add_a(condition(true));
  this->add_b(condition(true));
  this->led->update();
  this->led->blink_n(1, on, N_OFF, pause);
  expect_writes({true, false, true, false}, {on, pause, on});
  EXPECT_EQ(this->led->get_signal(), 0);
}

// The handover after an action starts the turns again: A, coming to hold, has one after B.
TEST_F(CycleTest, TheCountShownStoppingEndsAnAction) {
  FakeCondition *a = condition(true);
  this->add_a(a);
  this->add_b(condition(true));
  this->led->update();
  this->led->blink_slow();
  a->holds = false;
  const uint32_t stopped = millis();
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 1);
  EXPECT_EQ(this->led->get_state(), State::BLINK_N);
  a->holds = true;
  this->led->update();
  // Lit by the slow blink, so B's first write is its off.
  this->pin->writes.clear();
  expect_writes({false, true, false}, {ROUND, N_ON});
  EXPECT_GE(this->pin->writes[0].at - stopped, B_ON);
  EXPECT_LE(this->pin->writes[0].at - stopped, B_ON + this->late());
  EXPECT_EQ(this->led->get_signal(), 0);
}

// A pulse over A's first blink goes back to A's series, from the top, and A still hands over.
TEST_F(CycleTest, APulseKeepsTheTurns) {
  this->add_a(condition(true));
  this->add_b(condition(true));
  this->led->update();
  this->led->pulse();
  expect_writes({true, false, true, false, true, false, true}, {PULSE + N_ON, N_OFF, N_ON, N_PAUSE, B_ON, ROUND});
}

// B, after A, keeps its own pause; C, the last, ends the round.
TEST_F(CycleTest, ThreeCountsTakeTurnsAndOnlyTheLastEndsTheRound) {
  const uint32_t c_on = 450;
  this->add_a(condition(true));
  this->add_b(condition(true));
  this->led->add_signal(condition(true), State::BLINK_N, 1, c_on, N_OFF, N_PAUSE);
  this->led->update();
  expect_writes({true, false, true, false, true, false, true, false, true},
                {N_ON, N_OFF, N_ON, N_PAUSE, B_ON, B_PAUSE, c_on, ROUND});
  EXPECT_EQ(this->led->get_signal(), 0);
}

// B stops with no update since: the handover checks afresh and gives A its series again, not a
// flash of B.
TEST_F(CycleTest, ACountThatStoppedBeforeItsTurnIsSkipped) {
  FakeCondition *b = condition(true);
  this->add_a(condition(true));
  this->add_b(b);
  this->led->update();
  b->holds = false;
  expect_writes({true, false, true, false, true, false, true}, {N_ON, N_OFF, N_ON, N_PAUSE, N_ON, N_OFF});
  EXPECT_EQ(this->led->get_signal(), 0);
}

// The last one gone with no update since: the handover turns it off.
TEST_F(CycleTest, AHandoverWithNoneHoldingTurnsItOff) {
  FakeCondition *a = condition(true);
  this->add_a(a);
  this->led->update();
  a->holds = false;
  run_until_writes(4);
  run_for(N_PAUSE + this->late());
  EXPECT_EQ(this->led->get_signal(), -1);
  EXPECT_EQ(this->led->get_state(), State::OFF);
  EXPECT_EQ(this->pin->levels(), Levels({true, false, true, false}));
}

// FN pressed with no update since: the handover shows it, not a dark LED until the next update.
TEST_F(CycleTest, AHandoverFindsAStateSignalThatCameToHold) {
  FakeCondition *reset = condition();
  this->led->add_signal(reset, State::BLINK_FAST);
  this->add_a(condition(true));
  this->led->update();
  reset->holds = true;
  expect_writes({true, false, true, false, true, false}, {N_ON, N_OFF, N_ON, N_PAUSE, FAST_ON});
  EXPECT_EQ(this->led->get_signal(), 0);
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);
}

TEST_F(CycleTest, AStateSignalThatComesToHoldEndsAnAction) {
  FakeCondition *reset = condition();
  this->led->add_signal(reset, State::BLINK_FAST);
  this->add_a(condition(true));
  this->led->update();
  this->led->turn_on();
  reset->holds = true;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 0);
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);
}

TEST_F(CycleTest, DumpConfigNamesTheTurnsAndTheRoundPause) {
  CapturedLog &log = CapturedLog::instance();
  log.clear();
  this->led->dump_config();
  // Without signals there are no turns to take.
  EXPECT_FALSE(log.has("Signals"));
  EXPECT_FALSE(log.has("Round pause"));

  this->add_a(condition());
  this->add_b(condition());
  log.clear();
  this->led->dump_config();
  EXPECT_TRUE(log.has("Signals: 2, counts in turn"));
  EXPECT_TRUE(log.has("Round pause: 900 ms"));

  this->led->set_cycle(0);
  log.clear();
  this->led->dump_config();
  EXPECT_TRUE(log.has("Signals: 2, counts in turn"));
  EXPECT_FALSE(log.has("Round pause"));
}

// A change in what holds is logged; a handover is not, as with two codes one comes every few
// seconds.
TEST_F(CycleTest, TheLogNamesTheSignalsThatHoldWhenTheyChange) {
  const uint32_t t = 50;
  FakeCondition *b = condition(true);
  FakeCondition *d = condition(true);
  this->led->add_signal(condition(), State::BLINK_N, 1, t, t, t);
  this->led->add_signal(b, State::BLINK_N, 1, t, t, t);
  this->led->add_signal(condition(), State::BLINK_N, 1, t, t, t);
  this->led->add_signal(d, State::BLINK_N, 1, t, t, t);
  CapturedLog &log = CapturedLog::instance();
  log.clear();
  this->led->update();
  this->led->update();
  EXPECT_EQ(log.debug(), Lines({"Signals: 1, 3"}));

  // B's series hands over to D's.
  run_until_writes(3);
  ASSERT_EQ(this->led->get_signal(), 3);
  EXPECT_EQ(log.debug(), Lines({"Signals: 1, 3"}));

  d->holds = false;
  this->led->update();
  ASSERT_EQ(this->led->get_signal(), 1);
  b->holds = false;
  this->led->update();
  EXPECT_EQ(log.debug(), Lines({"Signals: 1, 3", "Signals: 1", "No signal", "State: OFF"}));
}

// More than the line has room for: cut short, never past its end.
TEST_F(CycleTest, ALongListOfSignalsIsCutShort) {
  std::string all;
  for (int i = 0; i < 40; i++) {
    this->led->add_signal(condition(true), State::BLINK_N, 1, N_ON, N_OFF, N_PAUSE);
    all += (i == 0 ? "" : ", ") + std::to_string(i);
  }
  CapturedLog &log = CapturedLog::instance();
  log.clear();
  this->led->update();
  ASSERT_EQ(log.debug().size(), 1u);
  const std::string &line = log.debug()[0];
  const std::string prefix = "Signals: ";
  ASSERT_EQ(line.compare(0, prefix.size(), prefix), 0) << line;
  const std::string list = line.substr(prefix.size());
  EXPECT_GT(list.size(), 10u) << line;
  EXPECT_LT(list.size(), all.size()) << line;
  EXPECT_EQ(all.compare(0, list.size(), list), 0) << line;
}

}  // namespace esphome::status_indicator::testing
