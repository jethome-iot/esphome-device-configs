#include <algorithm>
#include <string>
#include "common.h"
#include "esphome/components/logger/logger.h"

namespace esphome::status_indicator::testing {

using State = IndicatorState;
using Levels = std::vector<bool>;

TEST_F(StatusIndicatorTest, SetupSetsThePinUpAndWritesLow) {
  this->new_indicator();
  this->led->setup();
  EXPECT_TRUE(this->pin->set_up);
  EXPECT_EQ(this->pin->levels(), Levels({false}));
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

// An action that ran before setup is not undone by it.
TEST_F(StatusIndicatorTest, SetupWritesTheCurrentOutput) {
  this->new_indicator();
  this->led->turn_on();
  this->led->setup();
  EXPECT_TRUE(this->pin->set_up);
  ASSERT_FALSE(this->pin->writes.empty());
  EXPECT_TRUE(this->pin->writes.back().level);
  EXPECT_EQ(this->led->get_state(), State::ON);

  this->led->turn_off();
  EXPECT_FALSE(this->pin->writes.back().level);
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

TEST_F(StatusIndicatorTest, TurnOnAndOffWriteOnce) {
  this->led->turn_on();
  EXPECT_EQ(this->led->get_state(), State::ON);
  this->led->turn_off();
  EXPECT_EQ(this->led->get_state(), State::OFF);
  EXPECT_EQ(this->pin->levels(), Levels({true, false}));
}

TEST_F(StatusIndicatorTest, TheCurrentStateIsANoOp) {
  this->led->turn_on();
  this->led->set_state(State::ON);
  this->led->turn_on();
  EXPECT_EQ(this->pin->levels(), Levels({true}));
  this->led->turn_off();
  this->led->set_state(State::OFF);
  EXPECT_EQ(this->pin->levels(), Levels({true, false}));
}

// Asked again half way through its on phase, a blink keeps its timing instead of restarting.
TEST_F(StatusIndicatorTest, TheCurrentBlinkIsNotRestarted) {
  this->led->blink_slow();
  run_for(SLOW_ON / 2);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 1);
  this->led->set_state(State::BLINK_SLOW);
  this->led->blink_slow();
  run_until_writes(2);
  ASSERT_EQ(this->pin->levels(2), Levels({true, false}));
  // Restarted, it would stay lit a whole SLOW_ON from here: half of one longer in all.
  EXPECT_LE(this->pin->gap(1), SLOW_ON + this->late());
}

TEST_F(StatusIndicatorTest, BlinkSlowKeepsItsTimes) {
  this->led->blink_slow();
  EXPECT_EQ(this->led->get_state(), State::BLINK_SLOW);
  expect_writes({true, false, true}, {SLOW_ON, SLOW_OFF});
}

TEST_F(StatusIndicatorTest, BlinkFastKeepsItsTimes) {
  this->led->blink_fast();
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);
  expect_writes({true, false, true, false, true}, {FAST_ON, FAST_OFF, FAST_ON, FAST_OFF});
}

TEST_F(StatusIndicatorTest, SwitchingBlinksTakesTheNewTimes) {
  this->led->blink_fast();
  run_for(FAST_ON + FAST_OFF / 2);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 2);
  const uint32_t switched = millis();
  this->led->blink_slow();
  run_until_writes(5);
  // Fast on, fast off cut short by the slow blink's on phase, at once, then the slow times.
  ASSERT_EQ(this->pin->levels(5), Levels({true, false, true, false, true}));
  EXPECT_LE(this->pin->writes[2].at - switched, 1 + this->stalled());
  EXPECT_GE(this->pin->gap(3), SLOW_ON);
  EXPECT_LE(this->pin->gap(3), SLOW_ON + this->late());
  EXPECT_GE(this->pin->gap(4), SLOW_OFF);
  EXPECT_LE(this->pin->gap(4), SLOW_OFF + this->late());
}

// Nothing may fire into a state that no longer blinks.
TEST_F(StatusIndicatorTest, TurningOffStopsTheBlink) {
  this->led->blink_fast();
  run_for(FAST_ON / 2);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 1);
  const uint32_t off = millis();
  this->led->turn_off();
  // Past where the blink would have written its next off and on.
  run_for(FAST_ON + FAST_OFF + FAST_ON / 2);
  EXPECT_EQ(this->pin->levels(), Levels({true, false}));
  // Off at once, not when the on phase would have ended.
  EXPECT_LE(this->pin->writes[1].at - off, 1 + this->stalled());
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

TEST_F(StatusIndicatorTest, BlinkNCountsThenPausesThenRepeats) {
  this->led->blink_n(3, N_ON, N_OFF, N_PAUSE);
  EXPECT_EQ(this->led->get_state(), State::BLINK_N);
  expect_writes({true, false, true, false, true, false, true}, {N_ON, N_OFF, N_ON, N_OFF, N_ON, N_PAUSE});
  // After the last blink the pause takes the place of its off time, rather than following it.
  EXPECT_LT(this->pin->gap(6), N_PAUSE + N_OFF + this->stalled());
}

TEST_F(StatusIndicatorTest, ASecondBlinkNRestartsWithItsOwnTiming) {
  this->led->blink_n(3, N_ON, N_OFF, N_PAUSE);
  run_for(N_ON + N_OFF / 2);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 2);
  this->pin->writes.clear();
  // A longer on and a shorter pause than the first: neither fits the first one's bounds.
  this->led->blink_n(1, N_ON + 100, N_OFF, N_PAUSE - 200);
  expect_writes({true, false, true, false}, {N_ON + 100, N_PAUSE - 200, N_ON + 100});
}

TEST_F(StatusIndicatorTest, SetStateBlinkNRepeatsTheLastBlinkN) {
  this->led->blink_n(2, N_ON, N_OFF, N_PAUSE);
  this->led->turn_off();
  this->pin->writes.clear();
  this->led->set_state(State::BLINK_N);
  expect_writes({true, false, true, false, true}, {N_ON, N_OFF, N_ON, N_PAUSE});
  EXPECT_LT(this->pin->gap(4), N_PAUSE + N_OFF + this->stalled());
}

TEST_F(StatusIndicatorTest, TurnOnCancelsABlinkN) {
  this->led->blink_n(3, N_ON, N_OFF, N_PAUSE);
  run_for(N_ON + N_OFF / 2);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 2);
  this->led->turn_on();
  EXPECT_EQ(this->led->get_state(), State::ON);
  // Past where the sequence would have written its next on and off.
  run_for(N_OFF / 2 + N_ON + N_OFF / 2);
  EXPECT_EQ(this->pin->levels(), Levels({true, false, true}));
}

TEST_F(StatusIndicatorTest, BlinkNOfZeroTurnsOff) {
  this->led->blink_fast();
  this->led->blink_n(0);
  EXPECT_EQ(this->led->get_state(), State::OFF);
  // Past where the fast blink would have written its next off and on.
  run_for(FAST_ON + FAST_OFF + FAST_ON / 2);
  EXPECT_EQ(this->pin->levels(), Levels({true, false}));
}

TEST_F(StatusIndicatorTest, BlinkNWithAZeroTimeIsRefused) {
  this->led->blink_fast();
  this->led->blink_n(3, 0, N_OFF, N_PAUSE);
  this->led->blink_n(3, N_ON, 0, N_PAUSE);
  this->led->blink_n(3, N_ON, N_OFF, 0);
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);
  expect_writes({true, false, true}, {FAST_ON, FAST_OFF});
}

TEST_F(StatusIndicatorTest, APulseLightsForItsDurationAndGoesBackToOff) {
  this->led->pulse(PULSE - 200);
  EXPECT_EQ(this->led->get_state(), State::PULSE);
  expect_writes({true, false}, {PULSE - 200});
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

TEST_F(StatusIndicatorTest, APulseWithoutDurationTakesTheConfiguredOne) {
  this->led->pulse();
  expect_writes({true, false}, {PULSE});
}

TEST_F(StatusIndicatorTest, SetStatePulseIsAPulse) {
  this->led->set_state(State::PULSE);
  EXPECT_EQ(this->led->get_state(), State::PULSE);
  expect_writes({true, false}, {PULSE});
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

// Lit in the dark half of a blink, the pulse hands back to the blink, which starts over.
TEST_F(StatusIndicatorTest, APulseGoesBackToTheBlinkItInterrupted) {
  const uint32_t pulse = 300;
  this->led->blink_slow();
  run_for(SLOW_ON + SLOW_OFF / 2);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 2);
  this->led->pulse(pulse);
  run_until_writes(4);
  EXPECT_EQ(this->led->get_state(), State::BLINK_SLOW);
  ASSERT_EQ(this->pin->levels(4), Levels({true, false, true, false}));
  // The pulse and the blink's on phase run together.
  EXPECT_GE(this->pin->gap(3), pulse + SLOW_ON);
  EXPECT_LE(this->pin->gap(3), pulse + SLOW_ON + this->late());
}

// The second pulse must not take the first for the state to go back to.
TEST_F(StatusIndicatorTest, APulseOverAPulseGoesBackToTheStateBeforeBoth) {
  this->led->turn_on();
  this->led->pulse(400);
  run_for(200);
  SKIP_UNLESS_STILL(this->led->get_state() == State::PULSE);
  this->led->pulse(300);
  // Past the second pulse's end by as long again as the first had left.
  run_for(300 + 200);
  EXPECT_EQ(this->led->get_state(), State::ON);
  EXPECT_EQ(this->pin->levels(), Levels({true}));

  this->led->turn_off();
  this->pin->writes.clear();
  this->led->pulse(400);
  run_for(200);
  SKIP_UNLESS_STILL(this->pin->writes.size() == 1);
  const uint32_t second = millis();
  this->led->pulse(300);
  run_until_writes(2);
  EXPECT_EQ(this->led->get_state(), State::OFF);
  ASSERT_EQ(this->pin->levels(2), Levels({true, false}));
  // Timed from the second pulse: the first one's end, 200 ms after it, is gone.
  EXPECT_GE(this->pin->writes[1].at - second, 300u);
  EXPECT_LE(this->pin->writes[1].at - second, 300 + this->late());
}

TEST_F(StatusIndicatorTest, AnotherStateEndsAPulse) {
  this->led->pulse();
  this->led->blink_fast();
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);
  // Lit by the pulse, so the blink's first write is its off, well before the pulse would end.
  expect_writes({true, false, true}, {FAST_ON, FAST_OFF});
}

// Every config line logged since clear(). Registered once: the logger keeps its listeners.
class ConfigLog {
 public:
  std::vector<std::string> lines;

  static ConfigLog &instance() {
    static ConfigLog *log = [] {
      auto *l = new ConfigLog();
      logger::global_logger->add_log_callback(l, &ConfigLog::on_log);
      return l;
    }();
    return *log;
  }
  bool has(const char *needle) const {
    return std::any_of(this->lines.begin(), this->lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    if (level == ESPHOME_LOG_LEVEL_CONFIG)
      static_cast<ConfigLog *>(self)->lines.emplace_back(message, len);
  }
};

TEST_F(StatusIndicatorTest, DumpConfigNamesThePinAndTheTimes) {
  ConfigLog &log = ConfigLog::instance();
  log.lines.clear();
  this->led->dump_config();
  EXPECT_TRUE(log.has("Pin: recording pin"));
  EXPECT_TRUE(log.has("Slow blink: 400 ms on, 600 ms off"));
  EXPECT_TRUE(log.has("Fast blink: 150 ms on, 250 ms off"));
  EXPECT_TRUE(log.has("Pulse: 500 ms"));
}

}  // namespace esphome::status_indicator::testing
