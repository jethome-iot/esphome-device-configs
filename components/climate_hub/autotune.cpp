#include "autotune.h"
#include "param_table.h"

namespace esphome::climate_hub {

namespace {

struct RuleFactors {
  float kp;
  float ki;
  float kd;
};

// Upstream's factors, the five it prints: http://www.mstarlabs.com/control/znrule.html
RuleFactors factors_of(AutotuneRule rule) {
  switch (rule) {
    case AutotuneRule::ZN_PI:
      return {0.45f, 0.54f, 0.0f};
    case AutotuneRule::ZN_PID:
      return {0.6f, 1.2f, 0.075f};
    case AutotuneRule::PESSEN:
      return {0.7f, 1.75f, 0.105f};
    case AutotuneRule::SOME_OVERSHOOT:
      return {0.333f, 0.667f, 0.111f};
    case AutotuneRule::NO_OVERSHOOT:
      return {0.2f, 0.4f, 0.0625f};
  }
  return {0.45f, 0.54f, 0.0f};
}

// `value` held in its parameter's range; `clamped` set when that moved it, NaN included.
float clamp_gain(const char *key, float value, bool *clamped) {
  const float held = clamp_param(key, value);
  if (!(held == value))
    *clamped = true;
  return held;
}

}  // namespace

AutotuneRun::AutotuneRun(AutotuneDirection direction, AutotuneRule rule, const PidGains &gains, float setpoint,
                         uint32_t now_ms)
    : direction_(direction),
      rule_(rule),
      old_gains_(gains),
      setpoint_(setpoint),
      started_ms_(now_ms),
      last_switch_ms_(now_ms) {
  // One relay, full or nothing: d is half of 1 either way.
  if (direction == AutotuneDirection::HEAT) {
    this->tuner_.config(0.f, 1.f);
  } else {
    this->tuner_.config(-1.f, 0.f);
  }
  this->tuner_.set_noiseband(AUTOTUNE_NOISEBAND);
}

bool AutotuneRun::feed(float value, uint32_t now_ms) {
  const float output = this->tuner_.update(this->setpoint_, value, now_ms);
  if (this->tuner_.phase_count() != this->switches_) {
    this->switches_ = this->tuner_.phase_count();
    this->last_switch_ms_ = now_ms;
  }
  return this->direction_ == AutotuneDirection::HEAT ? output > 0.f : output < 0.f;
}

AutotuneEnd AutotuneRun::limit_reached(uint32_t now_ms) const {
  if (now_ms - this->started_ms_ >= AUTOTUNE_MAX_MS)
    return AutotuneEnd::TIMEOUT;
  if (now_ms - this->last_switch_ms_ >= AUTOTUNE_STALL_MS)
    return AutotuneEnd::NO_SWITCH;
  if (this->tuner_.noisy())
    return AutotuneEnd::NOISY;
  return AutotuneEnd::NONE;
}

PidGains AutotuneRun::result(bool *clamped) const {
  const RuleFactors f = factors_of(this->rule_);
  const PidAutotuner::Gains raw = this->tuner_.gains(f.kp, f.ki, f.kd);
  *clamped = false;
  PidGains gains;
  gains.kp = clamp_gain("kp", raw.kp, clamped);
  gains.ki = clamp_gain("ki", raw.ki, clamped);
  gains.kd = clamp_gain("kd", raw.kd, clamped);
  return gains;
}

void AutotuneRun::succeed(const PidGains &gains, bool clamped, bool persisted, uint32_t now_ms) {
  this->state_ = AutotuneState::SUCCEEDED;
  this->ended_ms_ = now_ms;
  this->new_gains_ = gains;
  this->clamped_ = clamped;
  this->persisted_ = persisted;
  this->asymmetric_ = !this->tuner_.symmetrical();
  this->uneven_ = this->tuner_.swing_ratio() < AUTOTUNE_EVEN_RATIO;
}

void AutotuneRun::fail(AutotuneEnd why, uint32_t now_ms) {
  this->state_ = AutotuneState::FAILED;
  this->reason_ = why;
  this->ended_ms_ = now_ms;
}

uint32_t AutotuneRun::elapsed_ms(uint32_t now_ms) const {
  return (this->running() ? now_ms : this->ended_ms_) - this->started_ms_;
}

}  // namespace esphome::climate_hub
