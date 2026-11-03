#include "status_indicator.h"
#include <cinttypes>
#include <cstdio>
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
  if (this->signals_.empty())
    this->stop_poller();
}

void StatusIndicator::add_signal(Condition<> *condition, IndicatorState state, uint8_t count, uint32_t on_ms,
                                 uint32_t off_ms, uint32_t pause_ms) {
  this->signals_.push_back({condition, state, count, on_ms, off_ms, pause_ms});
  this->holding_.push_back(false);
}

// Only a change is applied: the same signal again would restart its blink_n, and an action
// taken since it was shown stays until another one is.
void StatusIndicator::update() {
  if (this->cycle_) {
    this->update_cycle_();
    return;
  }
  int winner = -1;
  for (size_t i = 0; i < this->signals_.size(); i++) {
    if (this->signals_[i].condition->check()) {
      winner = static_cast<int>(i);
      break;
    }
  }
  if (winner == this->signal_)
    return;
  if (winner < 0) {
    ESP_LOGD(TAG, "No signal");
  } else {
    ESP_LOGD(TAG, "Signal %d", winner);
  }
  this->show_(winner);
}

// A count shown keeps its turn while it holds, so one that comes to hold waits for it; one
// that stops hands over at once.
void StatusIndicator::update_cycle_() {
  int state_winner = -1;
  bool changed = false;
  for (size_t i = 0; i < this->signals_.size(); i++) {
    // Past a state signal that holds, nothing else shows.
    bool holds = state_winner < 0 && this->signals_[i].condition->check();
    if (holds && this->signals_[i].count == 0)
      state_winner = static_cast<int>(i);
    if (holds != this->holding_[i]) {
      this->holding_[i] = holds;
      changed = true;
    }
  }
  if (changed)
    this->log_holding_();
  int winner = state_winner;
  if (winner < 0) {
    if (this->signal_ >= 0 && this->holding_[this->signal_])
      return;
    winner = this->next_after_(this->signal_);
  }
  if (winner != this->signal_)
    this->show_(winner);
}

int StatusIndicator::next_after_(int from) const {
  const int size = static_cast<int>(this->signals_.size());
  for (int step = 1; step <= size; step++) {
    int i = (from + step) % size;
    if (this->holding_[i] && this->signals_[i].count > 0)
      return i;
  }
  return -1;
}

// The handovers go unlogged: with two codes they come every few seconds.
void StatusIndicator::log_holding_() const {
  char list[64];
  size_t len = 0;
  for (size_t i = 0; i < this->signals_.size() && len < sizeof(list); i++) {
    if (this->holding_[i])
      len += snprintf(list + len, sizeof(list) - len, len == 0 ? "%u" : ", %u", static_cast<unsigned>(i));
  }
  if (len == 0) {
    ESP_LOGD(TAG, "No signal");
  } else {
    ESP_LOGD(TAG, "Signals: %s", list);
  }
}

void StatusIndicator::show_(int index) {
  this->signal_ = index;
  if (index < 0) {
    this->turn_off();
    return;
  }
  const Signal &signal = this->signals_[index];
  if (signal.count == 0) {
    this->set_state(signal.state);
  } else if (this->cycle_) {
    this->start_blink_n_(signal.count, signal.on_ms, signal.off_ms, signal.pause_ms);
    this->cycling_ = true;
  } else {
    this->blink_n(signal.count, signal.on_ms, signal.off_ms, signal.pause_ms);
  }
}

void StatusIndicator::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Status Indicator:\n"
                "  Slow blink: %" PRIu32 " ms on, %" PRIu32 " ms off\n"
                "  Fast blink: %" PRIu32 " ms on, %" PRIu32 " ms off\n"
                "  Pulse: %" PRIu32 " ms",
                this->slow_on_ms_, this->slow_off_ms_, this->fast_on_ms_, this->fast_off_ms_, this->pulse_duration_ms_);
  LOG_PIN("  Pin: ", this->pin_);
  if (!this->signals_.empty()) {
    ESP_LOGCONFIG(TAG, "  Signals: %u%s", static_cast<unsigned>(this->signals_.size()),
                  this->cycle_ ? ", counts in turn" : "");
    if (this->cycle_ && this->cycle_pause_ms_ > 0)
      ESP_LOGCONFIG(TAG, "  Round pause: %" PRIu32 " ms", this->cycle_pause_ms_);
    LOG_UPDATE_INTERVAL(this);
  }
}

void StatusIndicator::set_state(IndicatorState state) {
  if (state == IndicatorState::PULSE) {
    this->pulse();
    return;
  }
  if (state == this->state_)
    return;
  this->cancel_timeout(TIMER);
  this->cycling_ = false;
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
  ESP_LOGD(TAG, "Blink %u times, %" PRIu32 " ms on, %" PRIu32 " ms off, %" PRIu32 " ms pause", count, on_ms, off_ms,
           pause_ms);
  this->start_blink_n_(count, on_ms, off_ms, pause_ms);
}

void StatusIndicator::start_blink_n_(uint8_t count, uint32_t on_ms, uint32_t off_ms, uint32_t pause_ms) {
  this->cancel_timeout(TIMER);
  this->cycling_ = false;
  this->blink_count_ = count;
  this->blink_on_ms_ = on_ms;
  this->blink_off_ms_ = off_ms;
  this->blink_pause_ms_ = pause_ms;
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
    if (left > 1) {
      this->set_timeout(TIMER, this->blink_off_ms_, [this, left]() { this->blink_n_(static_cast<uint8_t>(left - 1)); });
      return;
    }
    uint32_t pause = this->blink_pause_ms_;
    if (this->cycling_ && this->cycle_pause_ms_ > 0) {
      // Wrapping round to an earlier count ends the round.
      int next = this->next_after_(this->signal_);
      if (next >= 0 && next < this->signal_)
        pause = this->cycle_pause_ms_;
    }
    this->set_timeout(TIMER, pause, [this]() { this->next_series_(); });
  });
}

// The next count's series in a cycle, else this one's again.
void StatusIndicator::next_series_() {
  int next = this->cycling_ ? this->next_after_(this->signal_) : -1;
  if (next >= 0 && next != this->signal_) {
    this->show_(next);
  } else {
    this->blink_n_(this->blink_count_);
  }
}

void StatusIndicator::set_output_(bool on) {
  if (on == this->output_)
    return;
  this->output_ = on;
  this->pin_->digital_write(on);
}

}  // namespace esphome::status_indicator
