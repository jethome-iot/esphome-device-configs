#include "common.h"
#include "esphome/core/automation.h"

namespace esphome::status_indicator::testing {

using State = IndicatorState;
using Levels = std::vector<bool>;

// Holds when the test says so, and counts how often it was asked.
class FakeCondition : public Condition<> {
 public:
  bool holds{false};
  int checks{0};

  bool check() override {
    this->checks++;
    return this->holds;
  }
};

// Counts the poller's calls.
class PolledIndicator : public StatusIndicator {
 public:
  int updates{0};

  void update() override {
    this->updates++;
    StatusIndicator::update();
  }
};

// Short, so a few polls fit in a test.
constexpr uint32_t INTERVAL = 50;

class SignalTest : public StatusIndicatorTest {
 protected:
  // Outlives the test, as the instance that checks it does.
  static FakeCondition *condition(bool holds = false) {
    static std::vector<std::unique_ptr<FakeCondition>> conditions;
    conditions.push_back(std::make_unique<FakeCondition>());
    conditions.back()->holds = holds;
    return conditions.back().get();
  }

  // Not the fixture's, which is set up already. Outlives the test: the poller points at it.
  static PolledIndicator *new_polled() {
    static std::vector<std::unique_ptr<RecordingPin>> pins;
    static std::vector<std::unique_ptr<PolledIndicator>> leds;
    pins.push_back(std::make_unique<RecordingPin>());
    leds.push_back(std::make_unique<PolledIndicator>());
    leds.back()->set_pin(pins.back().get());
    leds.back()->set_update_interval(INTERVAL);
    return leds.back().get();
  }
};

TEST_F(SignalTest, WithoutSignalsAnUpdateChangesNothing) {
  this->led->turn_on();
  this->led->update();
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), -1);
  EXPECT_EQ(this->led->get_state(), State::ON);
  EXPECT_EQ(this->pin->levels(), Levels({true}));
}

// None shown and none holding is no change: an action taken before any signal holds stays.
TEST_F(SignalTest, NoneHoldingAtFirstLeavesTheLedAlone) {
  this->led->add_signal(condition(), State::ON);
  this->led->add_signal(condition(), State::BLINK_N, 2, N_ON, N_OFF, N_PAUSE);
  this->led->blink_slow();
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), -1);
  EXPECT_EQ(this->led->get_state(), State::BLINK_SLOW);
  EXPECT_EQ(this->pin->levels(), Levels({true}));
}

TEST_F(SignalTest, TheFirstThatHoldsWins) {
  this->led->add_signal(condition(), State::BLINK_SLOW);
  this->led->add_signal(condition(true), State::ON);
  this->led->add_signal(condition(true), State::BLINK_FAST);
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 1);
  EXPECT_EQ(this->led->get_state(), State::ON);
}

// The index is the order added, whichever way the winner moves.
TEST_F(SignalTest, GetSignalFollowsTheWinner) {
  FakeCondition *first = condition();
  FakeCondition *second = condition();
  FakeCondition *third = condition(true);
  this->led->add_signal(first, State::ON);
  this->led->add_signal(second, State::BLINK_SLOW);
  this->led->add_signal(third, State::BLINK_FAST);
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 2);
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);

  first->holds = true;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 0);
  EXPECT_EQ(this->led->get_state(), State::ON);

  first->holds = false;
  second->holds = true;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 1);
  EXPECT_EQ(this->led->get_state(), State::BLINK_SLOW);

  second->holds = false;
  third->holds = false;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), -1);
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

// Each update asks from the top, the winner too, and none after it.
TEST_F(SignalTest, TheOnesAfterTheWinnerAreNotChecked) {
  FakeCondition *first = condition();
  FakeCondition *second = condition(true);
  FakeCondition *third = condition(true);
  this->led->add_signal(first, State::ON);
  this->led->add_signal(second, State::BLINK_SLOW);
  this->led->add_signal(third, State::BLINK_FAST);
  this->led->update();
  this->led->update();
  EXPECT_EQ(first->checks, 2);
  EXPECT_EQ(second->checks, 2);
  EXPECT_EQ(third->checks, 0);
}

TEST_F(SignalTest, AStateSignalSetsItsState) {
  this->led->add_signal(condition(true), State::BLINK_SLOW);
  this->led->update();
  EXPECT_EQ(this->led->get_state(), State::BLINK_SLOW);
  expect_writes({true, false, true}, {SLOW_ON, SLOW_OFF});
}

TEST_F(SignalTest, ACountSignalBlinksWithItsOwnTimes) {
  this->led->add_signal(condition(true), State::BLINK_N, 2, N_ON, N_OFF, N_PAUSE);
  this->led->update();
  EXPECT_EQ(this->led->get_state(), State::BLINK_N);
  expect_writes({true, false, true, false, true}, {N_ON, N_OFF, N_ON, N_PAUSE});
  EXPECT_LT(this->pin->gap(4), N_PAUSE + N_OFF + this->stalled());
}

// Lit by the slow blink, so the handover writes nothing: the first off comes N_ON after it,
// not SLOW_ON.
TEST_F(SignalTest, ANewWinnerTakesOverAtOnce) {
  FakeCondition *slow = condition(true);
  FakeCondition *code = condition();
  this->led->add_signal(slow, State::BLINK_SLOW);
  this->led->add_signal(code, State::BLINK_N, 2, N_ON, N_OFF, N_PAUSE);
  this->led->update();
  ASSERT_EQ(this->led->get_state(), State::BLINK_SLOW);
  slow->holds = false;
  code->holds = true;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 1);
  EXPECT_EQ(this->led->get_state(), State::BLINK_N);
  expect_writes({true, false, true, false, true}, {N_ON, N_OFF, N_ON, N_PAUSE});
}

// Asked again in its off phase, the blink_n keeps its count and timing instead of lighting at once.
TEST_F(SignalTest, TheSameWinnerAgainKeepsTheRhythm) {
  this->led->add_signal(condition(true), State::BLINK_N, 3, N_ON, N_OFF, N_PAUSE);
  this->led->update();
  run_for(N_ON + N_OFF / 2);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 2);
  this->led->update();
  this->led->update();
  expect_writes({true, false, true, false, true, false, true}, {N_ON, N_OFF, N_ON, N_OFF, N_ON, N_PAUSE});
}

// The state it shows is the one the LED is in already: a no-op, as set_state() is.
TEST_F(SignalTest, AStateSignalForTheCurrentStateKeepsTheRhythm) {
  this->led->blink_slow();
  run_for(SLOW_ON + SLOW_OFF / 2);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 2);
  this->led->add_signal(condition(true), State::BLINK_SLOW);
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 0);
  expect_writes({true, false, true}, {SLOW_ON, SLOW_OFF});
}

// Nothing may fire into the blink_n once no signal holds.
TEST_F(SignalTest, NoneHoldingAnyMoreTurnsItOff) {
  FakeCondition *code = condition(true);
  this->led->add_signal(code, State::BLINK_N, 2, N_ON, N_OFF, N_PAUSE);
  this->led->update();
  run_for(N_ON / 2);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 1);
  code->holds = false;
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), -1);
  EXPECT_EQ(this->led->get_state(), State::OFF);
  // Past where the blink would have written its next off and on.
  run_for(N_ON + N_OFF + N_ON / 2);
  EXPECT_EQ(this->pin->levels(), Levels({true, false}));
}

TEST_F(SignalTest, AnActionStaysUntilTheWinnerChanges) {
  FakeCondition *fast = condition(true);
  FakeCondition *slow = condition();
  this->led->add_signal(fast, State::BLINK_FAST);
  this->led->add_signal(slow, State::BLINK_SLOW);
  this->led->update();
  this->led->turn_on();
  this->led->update();
  this->led->update();
  EXPECT_EQ(this->led->get_signal(), 0);
  EXPECT_EQ(this->led->get_state(), State::ON);

  fast->holds = false;
  slow->holds = true;
  this->led->update();
  EXPECT_EQ(this->led->get_state(), State::BLINK_SLOW);

  this->led->turn_on();
  slow->holds = false;
  this->led->update();
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

// Through PollingComponent's own call_setup(), as the generated setup runs it.
TEST_F(SignalTest, SetupStopsThePollerWithoutSignals) {
  PolledIndicator *polled = new_polled();
  polled->call_setup();
  run_for(4 * INTERVAL);
  EXPECT_EQ(polled->updates, 0);
}

TEST_F(SignalTest, SetupKeepsThePollerWithSignals) {
  PolledIndicator *polled = new_polled();
  polled->add_signal(condition(true), State::ON);
  polled->call_setup();
  run_for(4 * INTERVAL);
  // Left running, it would poll through the tests after this one.
  polled->stop_poller();
  // One poll is enough to show it runs, and a stalled host may allow no more.
  EXPECT_GT(polled->updates, 0);
  EXPECT_EQ(polled->get_signal(), 0);
  EXPECT_EQ(polled->get_state(), State::ON);
}

TEST_F(SignalTest, DumpConfigCountsTheSignalsAndNamesTheInterval) {
  this->led->add_signal(condition(), State::ON);
  this->led->add_signal(condition(), State::BLINK_N, 2, N_ON, N_OFF, N_PAUSE);
  this->led->set_update_interval(500);
  ConfigLog &log = ConfigLog::instance();
  log.lines.clear();
  this->led->dump_config();
  EXPECT_TRUE(log.has("Pin: recording pin"));
  EXPECT_TRUE(log.has("Signals: 2"));
  EXPECT_TRUE(log.has("Update Interval: 0.500s"));
}

}  // namespace esphome::status_indicator::testing
