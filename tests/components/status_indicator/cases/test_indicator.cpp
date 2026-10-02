#include <algorithm>
#include <string>
#include "common.h"
#include "esphome/components/logger/logger.h"

namespace esphome::status_indicator::testing {

using State = IndicatorState;

TEST_F(StatusIndicatorTest, SetupSetsThePinUpAndWritesLow) {
  this->new_indicator();
  this->led->setup();
  EXPECT_TRUE(this->pin->set_up);
  EXPECT_EQ(this->pin->levels(), std::vector<bool>({false}));
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

TEST_F(StatusIndicatorTest, TurnOnAndOffWriteOnce) {
  this->led->turn_on();
  EXPECT_EQ(this->led->get_state(), State::ON);
  this->led->turn_off();
  EXPECT_EQ(this->led->get_state(), State::OFF);
  EXPECT_EQ(this->pin->levels(), std::vector<bool>({true, false}));
}

TEST_F(StatusIndicatorTest, TheCurrentStateIsANoOp) {
  this->led->turn_on();
  this->led->set_state(State::ON);
  this->led->turn_on();
  EXPECT_EQ(this->pin->levels(), std::vector<bool>({true}));
  this->led->turn_off();
  this->led->set_state(State::OFF);
  EXPECT_EQ(this->pin->levels(), std::vector<bool>({true, false}));
}

// Asked again half way through its on phase, a blink keeps its timing instead of restarting.
TEST_F(StatusIndicatorTest, TheCurrentBlinkIsNotRestarted) {
  this->led->blink_slow();
  run_for(SLOW_ON / 2);
  const uint32_t asked = millis();
  this->led->set_state(State::BLINK_SLOW);
  this->led->blink_slow();
  run_for(SLOW_ON);
  ASSERT_EQ(this->pin->levels(), std::vector<bool>({true, false}));
  EXPECT_LT(this->pin->writes[1].at - asked, SLOW_ON);
}

TEST_F(StatusIndicatorTest, BlinkSlowKeepsItsTimes) {
  this->led->blink_slow();
  EXPECT_EQ(this->led->get_state(), State::BLINK_SLOW);
  run_for(2 * (SLOW_ON + SLOW_OFF) + SLACK);
  expect_writes({true, false, true, false, true}, {SLOW_ON, SLOW_OFF, SLOW_ON, SLOW_OFF});
}

TEST_F(StatusIndicatorTest, BlinkFastKeepsItsTimes) {
  this->led->blink_fast();
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);
  run_for(2 * (FAST_ON + FAST_OFF) + SLACK);
  expect_writes({true, false, true, false, true}, {FAST_ON, FAST_OFF, FAST_ON, FAST_OFF});
}

TEST_F(StatusIndicatorTest, SwitchingBlinksTakesTheNewTimes) {
  this->led->blink_fast();
  run_for(FAST_ON + FAST_OFF / 2);
  this->led->blink_slow();
  run_for(SLOW_ON + SLOW_OFF + SLACK);
  // Fast on, fast off cut short by the slow blink's on phase, then the slow times.
  ASSERT_GE(this->pin->writes.size(), 5u);
  EXPECT_EQ(this->pin->levels(), std::vector<bool>({true, false, true, false, true}));
  EXPECT_GE(this->pin->gap(3), SLOW_ON);
  EXPECT_GE(this->pin->gap(4), SLOW_OFF);
}

// Nothing may fire into a state that no longer blinks.
TEST_F(StatusIndicatorTest, TurningOffStopsTheBlink) {
  this->led->blink_fast();
  run_for(FAST_ON / 2);
  this->led->turn_off();
  run_for(3 * (FAST_ON + FAST_OFF));
  EXPECT_EQ(this->pin->levels(), std::vector<bool>({true, false}));
  EXPECT_LT(this->pin->gap(1), FAST_ON);
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

// After the last blink the pause takes the place of its off time.
TEST_F(StatusIndicatorTest, BlinkNCountsThenPausesThenRepeats) {
  this->led->blink_n(3, 20, 30, 120);
  EXPECT_EQ(this->led->get_state(), State::BLINK_N);
  run_for(3 * 20 + 2 * 30 + 120 + SLACK);
  expect_writes({true, false, true, false, true, false, true}, {20, 30, 20, 30, 20, 120});
}

TEST_F(StatusIndicatorTest, ASecondBlinkNRestartsWithItsOwnTiming) {
  this->led->blink_n(3, 20, 30, 120);
  run_for(20 + 30 / 2);
  this->pin->writes.clear();
  this->led->blink_n(1, 25, 30, 60);
  run_for(25 + 60 + 25 + SLACK);
  expect_writes({true, false, true, false}, {25, 60, 25});
}

TEST_F(StatusIndicatorTest, SetStateBlinkNRepeatsTheLastBlinkN) {
  this->led->blink_n(2, 20, 30, 60);
  this->led->turn_off();
  this->pin->writes.clear();
  this->led->set_state(State::BLINK_N);
  run_for(20 + 30 + 20 + 60 + SLACK);
  expect_writes({true, false, true, false, true}, {20, 30, 20, 60});
}

TEST_F(StatusIndicatorTest, TurnOnCancelsABlinkN) {
  this->led->blink_n(3, 20, 30, 120);
  run_for(20 + 30 / 2);
  this->led->turn_on();
  EXPECT_EQ(this->led->get_state(), State::ON);
  run_for(3 * 20 + 2 * 30 + 120);
  EXPECT_EQ(this->pin->levels(), std::vector<bool>({true, false, true}));
}

TEST_F(StatusIndicatorTest, BlinkNOfZeroTurnsOff) {
  this->led->blink_fast();
  this->led->blink_n(0);
  EXPECT_EQ(this->led->get_state(), State::OFF);
  run_for(2 * (FAST_ON + FAST_OFF));
  EXPECT_EQ(this->pin->levels(), std::vector<bool>({true, false}));
}

TEST_F(StatusIndicatorTest, APulseLightsForItsDurationAndGoesBackToOff) {
  this->led->pulse(50);
  EXPECT_EQ(this->led->get_state(), State::PULSE);
  run_for(50 + SLACK);
  expect_writes({true, false}, {50});
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

TEST_F(StatusIndicatorTest, APulseWithoutDurationTakesTheConfiguredOne) {
  this->led->pulse();
  run_for(PULSE + SLACK);
  expect_writes({true, false}, {PULSE});
}

TEST_F(StatusIndicatorTest, SetStatePulseIsAPulse) {
  this->led->set_state(State::PULSE);
  EXPECT_EQ(this->led->get_state(), State::PULSE);
  run_for(PULSE + SLACK);
  expect_writes({true, false}, {PULSE});
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

// Lit in the dark half of a blink, the pulse hands back to the blink, which starts over.
TEST_F(StatusIndicatorTest, APulseGoesBackToTheBlinkItInterrupted) {
  this->led->blink_slow();
  run_for(SLOW_ON + SLOW_OFF / 2);
  this->led->pulse(30);
  run_for(30 + SLOW_ON + SLACK);
  EXPECT_EQ(this->led->get_state(), State::BLINK_SLOW);
  ASSERT_GE(this->pin->writes.size(), 4u);
  EXPECT_EQ(this->pin->levels()[2], true);
  EXPECT_EQ(this->pin->levels()[3], false);
  // The pulse and the blink's on phase run together.
  EXPECT_GE(this->pin->gap(3), 30 + SLOW_ON);
}

// The second pulse must not take the first for the state to go back to.
TEST_F(StatusIndicatorTest, APulseOverAPulseGoesBackToTheStateBeforeBoth) {
  this->led->turn_on();
  this->led->pulse(60);
  run_for(20);
  this->led->pulse(40);
  run_for(40 + SLACK);
  EXPECT_EQ(this->led->get_state(), State::ON);
  EXPECT_EQ(this->pin->levels(), std::vector<bool>({true}));

  this->led->turn_off();
  this->pin->writes.clear();
  this->led->pulse(60);
  run_for(20);
  this->led->pulse(40);
  run_for(60 + SLACK);
  EXPECT_EQ(this->led->get_state(), State::OFF);
  expect_writes({true, false}, {20 + 40});
}

TEST_F(StatusIndicatorTest, AnotherStateEndsAPulse) {
  this->led->pulse(50);
  this->led->blink_fast();
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);
  run_for(FAST_ON + FAST_OFF + SLACK);
  // Lit by the pulse, so the blink's first write is its off.
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
  EXPECT_TRUE(log.has("Slow blink: 60 ms on, 100 ms off"));
  EXPECT_TRUE(log.has("Fast blink: 20 ms on, 40 ms off"));
  EXPECT_TRUE(log.has("Pulse: 70 ms"));
}

}  // namespace esphome::status_indicator::testing
