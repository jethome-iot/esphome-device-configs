#pragma once

#include <cstdint>
#include <vector>
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

namespace esphome::status_indicator {

enum class IndicatorState : uint8_t {
  OFF = 0,
  ON = 1,
  BLINK_SLOW = 2,
  BLINK_FAST = 3,
  BLINK_N = 4,
  PULSE = 5,
};

class StatusIndicator : public PollingComponent {
 public:
  void set_pin(GPIOPin *pin) { this->pin_ = pin; }

  void set_slow_on_time(uint32_t ms) { this->slow_on_ms_ = ms; }
  void set_slow_off_time(uint32_t ms) { this->slow_off_ms_ = ms; }
  void set_fast_on_time(uint32_t ms) { this->fast_on_ms_ = ms; }
  void set_fast_off_time(uint32_t ms) { this->fast_off_ms_ = ms; }
  void set_pulse_duration(uint32_t ms) { this->pulse_duration_ms_ = ms; }

  // The current state is a no-op; PULSE is pulse() with the configured duration.
  void set_state(IndicatorState state);
  IndicatorState get_state() const { return this->state_; }

  void turn_off() { this->set_state(IndicatorState::OFF); }
  void turn_on() { this->set_state(IndicatorState::ON); }
  void blink_slow() { this->set_state(IndicatorState::BLINK_SLOW); }
  void blink_fast() { this->set_state(IndicatorState::BLINK_FAST); }

  // count blinks, then pause_ms dark in place of the last off_ms; again. A count of 0 turns it off.
  void blink_n(uint8_t count, uint32_t on_ms = 200, uint32_t off_ms = 200, uint32_t pause_ms = 1500);
  // On for duration_ms (0: the configured one), then back to the state it interrupted.
  void pulse(uint32_t duration_ms = 0);

  // Shown while its condition holds and no signal added before it does: blink_n with these
  // times when count is above 0, else state, which the schema keeps to ON, BLINK_SLOW and
  // BLINK_FAST.
  void add_signal(Condition<> *condition, IndicatorState state, uint8_t count = 0, uint32_t on_ms = 200,
                  uint32_t off_ms = 200, uint32_t pause_ms = 1500);
  // The signal shown, as its index in the order added; -1 for none.
  int get_signal() const { return this->signal_; }

  void setup() override;
  // Shows the first signal that holds, when it is another than the last one shown.
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  void set_output_(bool on);
  void apply_state_();
  void blink_(bool fast);
  void blink_n_(uint8_t left);

  struct Signal {
    Condition<> *condition;
    IndicatorState state;
    uint8_t count;
    uint32_t on_ms;
    uint32_t off_ms;
    uint32_t pause_ms;
  };
  std::vector<Signal> signals_;
  int signal_{-1};

  GPIOPin *pin_{nullptr};
  IndicatorState state_{IndicatorState::OFF};
  IndicatorState pre_pulse_state_{IndicatorState::OFF};
  bool output_{false};

  uint32_t slow_on_ms_{500};
  uint32_t slow_off_ms_{500};
  uint32_t fast_on_ms_{200};
  uint32_t fast_off_ms_{200};
  uint32_t pulse_duration_ms_{200};

  uint8_t blink_count_{3};
  uint32_t blink_on_ms_{200};
  uint32_t blink_off_ms_{200};
  uint32_t blink_pause_ms_{1500};
};

}  // namespace esphome::status_indicator
