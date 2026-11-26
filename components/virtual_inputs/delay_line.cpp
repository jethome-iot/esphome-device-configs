#include "delay_line.h"

namespace esphome::virtual_inputs {

void DelayLine::reset() {
  this->output_ = false;
  this->level_ = false;
  this->has_level_ = false;
  this->pending_ = false;
}

bool DelayLine::feed(bool level, uint32_t now) {
  if (this->has_level_ && level == this->level_)
    return false;
  return this->restart(level, now);
}

bool DelayLine::restart(bool level, uint32_t now) {
  this->level_ = level;
  this->has_level_ = true;
  return this->retarget_(now);
}

void DelayLine::forget_level() {
  this->has_level_ = false;
  this->pending_ = false;
}

// A source back at the output's level cancels the count; one that leaves it starts it afresh,
// so every new pulse restarts the off delay.
bool DelayLine::retarget_(uint32_t now) {
  this->pending_ = this->level_ != this->output_;
  this->since_ = now;
  return this->tick(now);
}

bool DelayLine::tick(uint32_t now) {
  if (!this->pending_)
    return false;
  const uint32_t wait = this->output_ ? this->off_ms_ : this->on_ms_;
  if (now - this->since_ < wait)
    return false;
  this->output_ = this->level_;
  this->pending_ = false;
  return true;
}

}  // namespace esphome::virtual_inputs
