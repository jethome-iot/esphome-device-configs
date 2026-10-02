#include "common.h"
#include "esphome/components/status_indicator/automation.h"

namespace esphome::status_indicator::testing {

using State = IndicatorState;
using ActionTest = StatusIndicatorTest;

TEST_F(ActionTest, TheStateActionsSetTheirState) {
  TurnOnAction<> turn_on;
  TurnOffAction<> turn_off;
  BlinkSlowAction<> blink_slow;
  BlinkFastAction<> blink_fast;
  for (Parented<StatusIndicator> *action :
       std::vector<Parented<StatusIndicator> *>{&turn_on, &turn_off, &blink_slow, &blink_fast})
    action->set_parent(this->led);

  turn_on.play_complex();
  EXPECT_EQ(this->led->get_state(), State::ON);
  blink_slow.play_complex();
  EXPECT_EQ(this->led->get_state(), State::BLINK_SLOW);
  blink_fast.play_complex();
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);
  turn_off.play_complex();
  EXPECT_EQ(this->led->get_state(), State::OFF);
}

TEST_F(ActionTest, BlinkNPassesItsFields) {
  BlinkNAction<> action;
  action.set_parent(this->led);
  action.set_count([]() -> uint8_t { return 2; });
  action.set_on_time([]() -> uint32_t { return 20; });
  action.set_off_time([]() -> uint32_t { return 30; });
  action.set_pause_time([]() -> uint32_t { return 60; });
  action.play_complex();
  EXPECT_EQ(this->led->get_state(), State::BLINK_N);
  run_for(20 + 30 + 20 + 60 + SLACK);
  expect_writes({true, false, true, false, true}, {20, 30, 20, 60});
}

// A lambda reads the trigger's argument, as `count: !lambda return x;` does.
TEST_F(ActionTest, BlinkNTakesItsCountFromTheTrigger) {
  BlinkNAction<int> action;
  action.set_parent(this->led);
  action.set_count([](int times) -> uint8_t { return times; });
  action.set_on_time([](int) -> uint32_t { return 20; });
  action.set_off_time([](int) -> uint32_t { return 30; });
  action.set_pause_time([](int) -> uint32_t { return 60; });
  action.play_complex(1);
  run_for(20 + 60 + 20 + SLACK);
  expect_writes({true, false, true, false}, {20, 60, 20});
}

TEST_F(ActionTest, PulseWithoutDurationTakesTheConfiguredOne) {
  PulseAction<> action;
  action.set_parent(this->led);
  action.play_complex();
  EXPECT_EQ(this->led->get_state(), State::PULSE);
  run_for(PULSE + SLACK);
  expect_writes({true, false}, {PULSE});
}

TEST_F(ActionTest, PulseTakesItsDuration) {
  PulseAction<> action;
  action.set_parent(this->led);
  action.set_duration([]() -> uint32_t { return 30; });
  action.play_complex();
  run_for(30 + SLACK);
  expect_writes({true, false}, {30});
}

TEST_F(ActionTest, SetStateSetsTheState) {
  SetStateAction<> action;
  action.set_parent(this->led);
  action.set_state([]() { return State::BLINK_FAST; });
  action.play_complex();
  EXPECT_EQ(this->led->get_state(), State::BLINK_FAST);
}

TEST_F(ActionTest, SetStateTakesItsStateFromTheTrigger) {
  SetStateAction<State> action;
  action.set_parent(this->led);
  action.set_state([](State state) { return state; });
  action.play_complex(State::ON);
  EXPECT_EQ(this->led->get_state(), State::ON);
  action.play_complex(State::PULSE);
  EXPECT_EQ(this->led->get_state(), State::PULSE);
}

}  // namespace esphome::status_indicator::testing
