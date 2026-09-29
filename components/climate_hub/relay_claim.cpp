#include "relay_claim.h"
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

namespace esphome::climate_hub {

void RelayClaim::set_dwell(uint32_t min_on_ms, uint32_t min_off_ms) {
  this->min_on_ms_ = min_on_ms;
  this->min_off_ms_ = min_off_ms;
}

bool RelayClaim::relay_state_() const {
#ifdef USE_SWITCH
  if (this->sw_ != nullptr)
    return this->sw_->state;
#endif
  return this->state_;
}

bool RelayClaim::request(bool want, uint32_t now_ms) {
  // The first request always lands: there is no dwell to honour before we owned it.
  if (!this->initialized_) {
    this->apply_(want, now_ms);
    return this->state_;
  }
  // Compared against the switch, not against our own belief: a bang-bang output sits at one
  // demand for hours, so a relay toggled from elsewhere would otherwise stay wrong until the
  // demand itself changed.
  const bool relay_agrees = this->relay_state_() == this->state_;
  if (want == this->state_ && relay_agrees)
    return this->state_;
  if (want == this->state_) {
    this->apply_(this->state_, this->last_change_ms_);
    return this->state_;
  }

  uint32_t held_for = now_ms - this->last_change_ms_;
  uint32_t floor = this->state_ ? this->min_on_ms_ : this->min_off_ms_;
  if (held_for < floor)
    return this->state_;

  this->apply_(want, now_ms);
  return this->state_;
}

void RelayClaim::force_off(uint32_t now_ms) {
  if (this->initialized_ && !this->state_ && !this->relay_state_())
    return;
  this->apply_(false, now_ms);
}

void RelayClaim::resume(const RelaySwitching &last) {
  this->state_ = last.on;
  this->last_change_ms_ = last.ms;
  this->initialized_ = true;
}

bool RelayClaim::last_switching(RelaySwitching *out) const {
  if (!this->initialized_)
    return false;
  out->on = this->state_;
  out->ms = this->last_change_ms_;
  return true;
}

void RelayClaim::apply_(bool on, uint32_t now_ms) {
  this->state_ = on;
  this->last_change_ms_ = now_ms;
  this->initialized_ = true;
#ifdef USE_SWITCH
  if (this->sw_ == nullptr)
    return;
  if (on) {
    this->sw_->turn_on();
  } else {
    this->sw_->turn_off();
  }
#endif
}

}  // namespace esphome::climate_hub
