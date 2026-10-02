#include "pid_core.h"
#include <algorithm>
#include <cmath>

namespace esphome::climate_hub {

void SampleWindow::reserve(size_t samples) {
  if (samples <= this->values_.size())
    return;
  std::vector<float> grown(samples);
  for (size_t i = 0; i < this->count_; i++)
    grown[i] = this->values_[(this->head_ + i) % this->values_.size()];
  this->values_.swap(grown);
  this->head_ = 0;
}

float SampleWindow::push_average(float value, size_t samples) {
  if (samples <= 1) {
    this->clear();
    return value;
  }
  this->reserve(samples);
  const size_t capacity = this->values_.size();
  while (this->count_ >= samples) {
    this->head_ = (this->head_ + 1) % capacity;
    this->count_--;
  }
  this->values_[(this->head_ + this->count_) % capacity] = value;
  this->count_++;

  float sum = 0.f;
  for (size_t i = 0; i < this->count_; i++)
    sum += this->values_[(this->head_ + i) % capacity];
  return sum / static_cast<float>(this->count_);
}

void PidCore::set_gains(float kp, float ki, float kd) {
  this->kp_ = kp;
  this->ki_ = ki;
  this->kd_ = kd;
}

void PidCore::set_integral_limits(float min_integral, float max_integral) {
  this->min_integral_ = min_integral;
  this->max_integral_ = max_integral;
  // A kept integral moves on from inside the new limits, not from where the old ones left it.
  this->clamp_integral_();
  this->integral_term_ = this->accumulated_integral_;
}

void PidCore::clamp_integral_() {
  if (!std::isnan(this->min_integral_) && this->accumulated_integral_ < this->min_integral_)
    this->accumulated_integral_ = this->min_integral_;
  if (!std::isnan(this->max_integral_) && this->accumulated_integral_ > this->max_integral_)
    this->accumulated_integral_ = this->max_integral_;
}

void PidCore::set_samples(int output_samples, int derivative_samples) {
  this->output_samples_ = output_samples < 1 ? 1 : output_samples;
  this->derivative_samples_ = derivative_samples < 1 ? 1 : derivative_samples;
  this->reserve_windows_();
}

void PidCore::set_deadband(float threshold_low, float threshold_high, float kp_multiplier, float ki_multiplier,
                           float kd_multiplier, int output_samples) {
  this->threshold_low_ = threshold_low;
  this->threshold_high_ = threshold_high;
  this->kp_multiplier_ = kp_multiplier;
  this->ki_multiplier_ = ki_multiplier;
  this->kd_multiplier_ = kd_multiplier;
  this->deadband_output_samples_ = output_samples < 1 ? 1 : output_samples;
  this->reserve_windows_();
}

// Sized as a controller starts, so the loop itself allocates nothing; a window of one holds
// nothing at all.
void PidCore::reserve_windows_() {
  if (this->derivative_samples_ > 1)
    this->derivative_window_.reserve(static_cast<size_t>(this->derivative_samples_));
  const int output = std::max(this->output_samples_, this->deadband_output_samples_);
  if (output > 1)
    this->output_window_.reserve(static_cast<size_t>(output));
}

void PidCore::reset() {
  this->accumulated_integral_ = 0.f;
  this->previous_error_ = 0.f;
  this->has_previous_setpoint_ = false;
  this->error_ = 0.f;
  this->proportional_term_ = 0.f;
  this->integral_term_ = 0.f;
  this->derivative_term_ = 0.f;
  this->derivative_window_.clear();
  this->output_window_.clear();
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
  return this->output_window_.push_average(output, static_cast<size_t>(samples));
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
  this->clamp_integral_();
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

  derivative = this->derivative_window_.push_average(derivative, static_cast<size_t>(this->derivative_samples_));
  this->derivative_term_ = this->kd_ * derivative;
  if (this->in_deadband())
    this->derivative_term_ *= this->kd_multiplier_;
}

}  // namespace esphome::climate_hub
