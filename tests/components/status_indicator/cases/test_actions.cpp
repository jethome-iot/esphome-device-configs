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
  action.set_on_time([]() -> uint32_t { return N_ON; });
  action.set_off_time([]() -> uint32_t { return N_OFF; });
  action.set_pause_time([]() -> uint32_t { return N_PAUSE; });
  action.play_complex();
  EXPECT_EQ(this->led->get_state(), State::BLINK_N);
  expect_writes({true, false, true, false, true}, {N_ON, N_OFF, N_ON, N_PAUSE});
  EXPECT_LT(this->pin->gap(4), N_PAUSE + N_OFF + this->stalled());
}

// A lambda reads the trigger's argument, as `count: !lambda return x;` does. One blink, so the
// pause follows the first one; a second blink would have shown its off time there.
TEST_F(ActionTest, BlinkNTakesItsCountFromTheTrigger) {
  BlinkNAction<int> action;
  action.set_parent(this->led);
  action.set_count([](int times) -> uint8_t { return times; });
  action.set_on_time([](int) -> uint32_t { return N_ON; });
  action.set_off_time([](int) -> uint32_t { return N_OFF; });
  action.set_pause_time([](int) -> uint32_t { return N_PAUSE; });
  action.play_complex(1);
  expect_writes({true, false, true}, {N_ON, N_PAUSE});
}

TEST_F(ActionTest, PulseWithoutDurationTakesTheConfiguredOne) {
  PulseAction<> action;
  action.set_parent(this->led);
  action.play_complex();
  EXPECT_EQ(this->led->get_state(), State::PULSE);
  expect_writes({true, false}, {PULSE});
}

TEST_F(ActionTest, PulseTakesItsDuration) {
  PulseAction<> action;
  action.set_parent(this->led);
  action.set_duration([]() -> uint32_t { return PULSE - 200; });
  action.play_complex();
  expect_writes({true, false}, {PULSE - 200});
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
