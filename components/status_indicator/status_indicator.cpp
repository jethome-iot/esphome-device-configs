#include "status_indicator.h"
#include <cinttypes>
#include "esphome/core/log.h"

namespace esphome::status_indicator {

static const char *const TAG = "status_indicator";
// Every phase of every state runs under this one name, so cancelling it stops any of them.
static const char *const TIMER = "blink";

static const LogString *state_to_string(IndicatorState state) {
  switch (state) {
    case IndicatorState::OFF:
      return LOG_STR("OFF");
    case IndicatorState::ON:
      return LOG_STR("ON");
    case IndicatorState::BLINK_SLOW:
      return LOG_STR("BLINK_SLOW");
    case IndicatorState::BLINK_FAST:
      return LOG_STR("BLINK_FAST");
    case IndicatorState::BLINK_N:
      return LOG_STR("BLINK_N");
    case IndicatorState::PULSE:
      return LOG_STR("PULSE");
  }
  return LOG_STR("UNKNOWN");
}

void StatusIndicator::setup() {
  this->pin_->setup();
  // Off, unless an action ran before setup.
  this->pin_->digital_write(this->output_);
}

void StatusIndicator::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Status Indicator:\n"
                "  Slow blink: %" PRIu32 " ms on, %" PRIu32 " ms off\n"
                "  Fast blink: %" PRIu32 " ms on, %" PRIu32 " ms off\n"
                "  Pulse: %" PRIu32 " ms",
                this->slow_on_ms_, this->slow_off_ms_, this->fast_on_ms_, this->fast_off_ms_, this->pulse_duration_ms_);
  LOG_PIN("  Pin: ", this->pin_);
}

void StatusIndicator::set_state(IndicatorState state) {
  if (state == IndicatorState::PULSE) {
    this->pulse();
    return;
  }
  if (state == this->state_)
    return;
  this->cancel_timeout(TIMER);
  ESP_LOGD(TAG, "State: %s", LOG_STR_ARG(state_to_string(state)));
  this->state_ = state;
  this->apply_state_();
}

void StatusIndicator::blink_n(uint8_t count, uint32_t on_ms, uint32_t off_ms, uint32_t pause_ms) {
  if (count == 0) {
    this->turn_off();
    return;
  }
  // The schema refuses a zero time; one from a lambda would reschedule on every loop pass.
  if (on_ms == 0 || off_ms == 0 || pause_ms == 0) {
    ESP_LOGW(TAG, "Blink N refused: every time must be above zero");
    return;
  }
  this->cancel_timeout(TIMER);
  this->blink_count_ = count;
  this->blink_on_ms_ = on_ms;
  this->blink_off_ms_ = off_ms;
  this->blink_pause_ms_ = pause_ms;
  ESP_LOGD(TAG, "Blink %u times, %" PRIu32 " ms on, %" PRIu32 " ms off, %" PRIu32 " ms pause", count, on_ms, off_ms,
           pause_ms);
  this->state_ = IndicatorState::BLINK_N;
  this->blink_n_(count);
}

void StatusIndicator::pulse(uint32_t duration_ms) {
  if (duration_ms == 0)
    duration_ms = this->pulse_duration_ms_;
  this->cancel_timeout(TIMER);
  // A pulse over a pulse goes back to what the first one interrupted.
  if (this->state_ != IndicatorState::PULSE)
    this->pre_pulse_state_ = this->state_;
  this->state_ = IndicatorState::PULSE;
  this->set_output_(true);
  this->set_timeout(TIMER, duration_ms, [this]() {
    this->state_ = this->pre_pulse_state_;
    this->apply_state_();
  });
}

void StatusIndicator::apply_state_() {
  switch (this->state_) {
    case IndicatorState::OFF:
      this->set_output_(false);
      break;
    case IndicatorState::ON:
      this->set_output_(true);
      break;
    case IndicatorState::BLINK_SLOW:
      this->blink_(false);
      break;
    case IndicatorState::BLINK_FAST:
      this->blink_(true);
      break;
    case IndicatorState::BLINK_N:
      this->blink_n_(this->blink_count_);
      break;
    case IndicatorState::PULSE:
      // Never applied: set_state() hands it to pulse(), which never goes back to it.
      break;
  }
}

// Captures stay within std::function's inline storage on 32-bit targets, so no phase allocates.
void StatusIndicator::blink_(bool fast) {
  this->set_output_(true);
  this->set_timeout(TIMER, fast ? this->fast_on_ms_ : this->slow_on_ms_, [this, fast]() {
    this->set_output_(false);
    this->set_timeout(TIMER, fast ? this->fast_off_ms_ : this->slow_off_ms_, [this, fast]() { this->blink_(fast); });
  });
}

void StatusIndicator::blink_n_(uint8_t left) {
  this->set_output_(true);
  this->set_timeout(TIMER, this->blink_on_ms_, [this, left]() {
    this->set_output_(false);
    bool last = left <= 1;
    uint8_t next = last ? this->blink_count_ : static_cast<uint8_t>(left - 1);
    this->set_timeout(TIMER, last ? this->blink_pause_ms_ : this->blink_off_ms_,
                      [this, next]() { this->blink_n_(next); });
  });
}

void StatusIndicator::set_output_(bool on) {
  if (on == this->output_)
    return;
  this->output_ = on;
  this->pin_->digital_write(on);
}

}  // namespace esphome::status_indicator
