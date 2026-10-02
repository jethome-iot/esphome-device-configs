#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "status_indicator.h"

namespace esphome::status_indicator {

template<typename... Ts> class TurnOnAction final : public Action<Ts...>, public Parented<StatusIndicator> {
 public:
  void play(const Ts &...x) override { this->parent_->turn_on(); }
};

template<typename... Ts> class TurnOffAction final : public Action<Ts...>, public Parented<StatusIndicator> {
 public:
  void play(const Ts &...x) override { this->parent_->turn_off(); }
};

template<typename... Ts> class BlinkSlowAction final : public Action<Ts...>, public Parented<StatusIndicator> {
 public:
  void play(const Ts &...x) override { this->parent_->blink_slow(); }
};

template<typename... Ts> class BlinkFastAction final : public Action<Ts...>, public Parented<StatusIndicator> {
 public:
  void play(const Ts &...x) override { this->parent_->blink_fast(); }
};

// Codegen sets every field: the schema carries the defaults.
template<typename... Ts> class BlinkNAction final : public Action<Ts...>, public Parented<StatusIndicator> {
 public:
  TEMPLATABLE_VALUE(uint8_t, count)
  TEMPLATABLE_VALUE(uint32_t, on_time)
  TEMPLATABLE_VALUE(uint32_t, off_time)
  TEMPLATABLE_VALUE(uint32_t, pause_time)

  void play(const Ts &...x) override {
    this->parent_->blink_n(this->count_.value(x...), this->on_time_.value(x...), this->off_time_.value(x...),
                           this->pause_time_.value(x...));
  }
};

// Without a duration, value() is 0 and the pulse takes the component's pulse_duration.
template<typename... Ts> class PulseAction final : public Action<Ts...>, public Parented<StatusIndicator> {
 public:
  TEMPLATABLE_VALUE(uint32_t, duration)

  void play(const Ts &...x) override { this->parent_->pulse(this->duration_.value(x...)); }
};

template<typename... Ts> class SetStateAction final : public Action<Ts...>, public Parented<StatusIndicator> {
 public:
  TEMPLATABLE_VALUE(IndicatorState, state)

  void play(const Ts &...x) override { this->parent_->set_state(this->state_.value(x...)); }
};

}  // namespace esphome::status_indicator
