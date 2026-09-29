#include "pid_core.h"
#include <cmath>

namespace esphome::climate_hub {

void PidCore::set_gains(float kp, float ki, float kd) {
  this->kp_ = kp;
  this->ki_ = ki;
  this->kd_ = kd;
}

void PidCore::set_integral_limits(float min_integral, float max_integral) {
  this->min_integral_ = min_integral;
  this->max_integral_ = max_integral;
}

void PidCore::set_samples(int output_samples, int derivative_samples) {
  this->output_samples_ = output_samples < 1 ? 1 : output_samples;
  this->derivative_samples_ = derivative_samples < 1 ? 1 : derivative_samples;
}

void PidCore::set_deadband(float threshold_low, float threshold_high, float kp_multiplier, float ki_multiplier,
                           float kd_multiplier, int output_samples) {
  this->threshold_low_ = threshold_low;
  this->threshold_high_ = threshold_high;
  this->kp_multiplier_ = kp_multiplier;
  this->ki_multiplier_ = ki_multiplier;
  this->kd_multiplier_ = kd_multiplier;
  this->deadband_output_samples_ = output_samples < 1 ? 1 : output_samples;
}

void PidCore::reset() {
  this->accumulated_integral_ = 0.f;
  this->previous_error_ = 0.f;
  this->has_previous_setpoint_ = false;
  this->error_ = 0.f;
  this->proportional_term_ = 0.f;
  this->integral_term_ = 0.f;
  this->derivative_term_ = 0.f;
  this->derivative_list_.clear();
  this->output_list_.clear();
}

// A zero-width deadband (both thresholds 0) can never be inside, which is what switches the
// deadband treatment off entirely.
bool PidCore::in_deadband() const {
  float err = -this->error_;
  return this->threshold_low_ < err && err < this->threshold_high_;
}

float PidCore::update(float setpoint, float process_value, float dt_s) {
  this->error_ = setpoint - process_value;

  this->calculate_proportional_term_();
  this->calculate_integral_term_(dt_s);
  this->calculate_derivative_term_(setpoint, dt_s);

  float output = this->proportional_term_ + this->integral_term_ + this->derivative_term_;
  int samples = this->in_deadband() ? this->deadband_output_samples_ : this->output_samples_;
  return weighted_average(this->output_list_, output, samples);
}

void PidCore::calculate_proportional_term_() {
  this->proportional_term_ = this->kp_ * this->error_;
  if (this->in_deadband()) {
    this->proportional_term_ *= this->kp_multiplier_;
    return;
  }
  // Offsetting by the threshold keeps the term continuous across the deadband edge.
  float threshold = this->error_ < 0 ? this->threshold_high_ : this->threshold_low_;
  this->proportional_term_ += (threshold - this->kp_multiplier_ * threshold) * this->kp_;
}

void PidCore::calculate_integral_term_(float dt_s) {
  float new_integral = this->error_ * dt_s * this->ki_;
  this->accumulated_integral_ += this->in_deadband() ? new_integral * this->ki_multiplier_ : new_integral;

  if (!std::isnan(this->min_integral_) && this->accumulated_integral_ < this->min_integral_)
    this->accumulated_integral_ = this->min_integral_;
  if (!std::isnan(this->max_integral_) && this->accumulated_integral_ > this->max_integral_)
    this->accumulated_integral_ = this->max_integral_;

  this->integral_term_ = this->accumulated_integral_;
}

void PidCore::calculate_derivative_term_(float setpoint, float dt_s) {
  float derivative = 0.f;
  if (dt_s != 0.f) {
    // A setpoint move is not an error change; subtracting it out avoids a kick.
    if (this->has_previous_setpoint_ && this->previous_setpoint_ != setpoint)
      this->previous_error_ -= this->previous_setpoint_ - setpoint;
    derivative = (this->error_ - this->previous_error_) / dt_s;
  }
  this->previous_error_ = this->error_;
  this->previous_setpoint_ = setpoint;
  this->has_previous_setpoint_ = true;

  derivative = weighted_average(this->derivative_list_, derivative, this->derivative_samples_);
  this->derivative_term_ = this->kd_ * derivative;
  if (this->in_deadband())
    this->derivative_term_ *= this->kd_multiplier_;
}

float PidCore::weighted_average(std::deque<float> &list, float new_value, int samples) {
  if (samples == 1) {
    list.clear();
    return new_value;
  }
  list.push_front(new_value);
  while (samples > 0 && list.size() > static_cast<size_t>(samples))
    list.pop_back();

  float sum = 0.f;
  for (float v : list)
    sum += v;
  return sum / list.size();
}

}  // namespace esphome::climate_hub
