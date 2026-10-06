#include "duty_cycler.h"
#include <cmath>

namespace esphome::climate_hub {

void DutyCycler::set_period(uint32_t period_ms) {
  if (period_ms < 1)
    period_ms = 1;
  if (period_ms == this->period_ms_)
    return;
  this->period_ms_ = period_ms;
  this->started_ = false;
}

void DutyCycler::set_duty(float duty) {
  if (std::isnan(duty)) {
    this->duty_ = 0.f;
    return;
  }
  this->duty_ = duty < 0.f ? 0.f : (duty > 1.f ? 1.f : duty);
}

void DutyCycler::start(uint64_t now_ms) {
  this->period_start_ms_ = now_ms;
  this->started_ = true;
}

bool DutyCycler::update(uint64_t now_ms) {
  if (!this->started_)
    this->start(now_ms);

  const uint32_t period = this->period_ms_ == 0 ? 1 : this->period_ms_;
  uint64_t elapsed = now_ms - this->period_start_ms_;
  if (elapsed >= period) {
    elapsed %= period;
    this->period_start_ms_ = now_ms - elapsed;
  }

  // The rails are exact: a duty of 0 or 1 never blips the relay at a period edge.
  if (this->duty_ <= 0.f)
    return false;
  if (this->duty_ >= 1.f)
    return true;
  return static_cast<float>(elapsed) < this->duty_ * static_cast<float>(period);
}

}  // namespace esphome::climate_hub
