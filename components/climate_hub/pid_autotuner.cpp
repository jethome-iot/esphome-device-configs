#include "pid_autotuner.h"
#include <algorithm>
#include <numbers>

namespace esphome::climate_hub {

// The relay drives the room around the target and the detectors measure the period and the
// amplitude of the swing it makes: Ku = 4d / (πa), where d is half the relay's span and a half
// the peak-to-peak swing, and Pu the mean period. upstream's pid_autotuner.cpp tells the story.

void PidAutotuner::config(float output_min, float output_max) {
  this->relay_.output_negative = std::max(this->relay_.output_negative, output_min);
  this->relay_.output_positive = std::min(this->relay_.output_positive, output_max);
}

void PidAutotuner::set_noiseband(float noiseband) {
  this->relay_.noiseband = noiseband;
  // A quarter of the relay's band keeps sensor noise from counting as a crossing.
  this->frequency_.noiseband = noiseband / 4;
}

float PidAutotuner::update(float setpoint, float process_value, uint32_t now_ms) {
  if (this->finished_)
    return 0.f;
  const float error = setpoint - process_value;
  const float output = this->relay_.update(error);
  this->frequency_.update(now_ms, error);
  Extreme ended;
  if (this->amplitude_.update(error, this->relay_.state, now_ms, &ended))
    this->extremes_.push_back(ended);

  // Upstream waits up to six more phases while its checks fail, but sets the phase to wait
  // from on the very pass it would wait on, and goes on: it stops here, at the first pass with
  // enough data, and so does this.
  if (!this->frequency_.has_enough_data() || !this->amplitude_.has_enough_data())
    return output;

  const float osc_ampl = this->amplitude_.mean_amplitude();
  const float d = (this->relay_.output_positive - this->relay_.output_negative) / 2.0f;
  this->ku_ = 4.0f * d / (std::numbers::pi_v<float> * osc_ampl);
  this->pu_ = this->frequency_.mean_period_s();
  this->finished_ = true;
  return output;
}

PidAutotuner::Gains PidAutotuner::gains(float kp_factor, float ki_factor, float kd_factor) const {
  return {
      .kp = kp_factor * this->ku_,
      .ki = ki_factor * this->ku_ / this->pu_,
      .kd = kd_factor * this->ku_ * this->pu_,
  };
}

// Upstream's convergence check holds whatever the data, since no swing outgrows the extremes it
// is measured between; the swings measured against each other say whether something disturbed
// the room.
float PidAutotuner::swing_ratio() const {
  if (this->extremes_.size() < 3)
    return NAN;
  float smallest = INFINITY;
  float largest = 0.f;
  // The first phase began wherever the room was, not at a switching point.
  for (size_t i = 2; i < this->extremes_.size(); i++) {
    const float swing = std::fabs(this->extremes_[i].error - this->extremes_[i - 1].error);
    smallest = std::min(smallest, swing);
    largest = std::max(largest, swing);
  }
  return largest > 0.f ? smallest / largest : NAN;
}

// --- RelayFunction ---

float PidAutotuner::RelayFunction::update(float error) {
  if (this->state == INIT)
    this->state = error > this->noiseband ? POSITIVE : NEGATIVE;
  bool change = false;
  if (this->state == POSITIVE && error < -this->noiseband) {
    this->state = NEGATIVE;
    change = true;
  } else if (this->state == NEGATIVE && error > this->noiseband) {
    this->state = POSITIVE;
    change = true;
  }
  if (change)
    this->phase_count++;
  return this->state == POSITIVE ? this->output_positive : this->output_negative;
}

float PidAutotuner::RelayFunction::current_target_error() const {
  if (this->state == INIT)
    return 0.f;
  return this->state == POSITIVE ? -this->noiseband : this->noiseband;
}

// --- FrequencyDetector ---

void PidAutotuner::FrequencyDetector::update(uint32_t now_ms, float error) {
  if (this->state == INIT)
    this->state = error > this->noiseband ? POSITIVE : NEGATIVE;
  bool crossing = false;
  if (this->state == POSITIVE && error < -this->noiseband) {
    this->state = NEGATIVE;
    crossing = true;
  } else if (this->state == NEGATIVE && error > this->noiseband) {
    this->state = POSITIVE;
    crossing = true;
  }
  if (!crossing)
    return;
  if (this->crossed)
    this->intervals.push_back(now_ms - this->last_crossing_ms);
  this->crossed = true;
  this->last_crossing_ms = now_ms;
}

float PidAutotuner::FrequencyDetector::mean_period_s() const {
  float sum = 0.0f;
  for (uint32_t v : this->intervals)
    sum += v;
  const float mean_value = sum / this->intervals.size();
  // Two crossings per period, in ms.
  return mean_value / 1000 * 2;
}

bool PidAutotuner::FrequencyDetector::symmetrical() const {
  if (this->intervals.empty())
    return false;
  uint32_t max_interval = this->intervals[0];
  uint32_t min_interval = this->intervals[0];
  for (uint32_t interval : this->intervals) {
    max_interval = std::max(max_interval, interval);
    min_interval = std::min(min_interval, interval);
  }
  const float ratio = min_interval / float(max_interval);
  return ratio >= 0.66f;
}

// --- AmplitudeDetector ---

bool PidAutotuner::AmplitudeDetector::update(float error, RelayFunction::State relay_state, uint32_t now_ms,
                                             Extreme *ended) {
  bool phase_ended = false;
  if (relay_state != this->last_relay_state) {
    // The peak lags the switch: a positive phase holds the largest error, a negative the smallest.
    if (this->last_relay_state == RelayFunction::POSITIVE) {
      this->phase_maxs.push_back(this->phase_max);
      *ended = {this->phase_max_ms, this->phase_max};
      phase_ended = true;
    } else if (this->last_relay_state == RelayFunction::NEGATIVE) {
      this->phase_mins.push_back(this->phase_min);
      *ended = {this->phase_min_ms, this->phase_min};
      phase_ended = true;
    }
    this->phase_min = error;
    this->phase_max = error;
    this->phase_min_ms = now_ms;
    this->phase_max_ms = now_ms;
  }
  this->last_relay_state = relay_state;

  if (error < this->phase_min) {
    this->phase_min = error;
    this->phase_min_ms = now_ms;
  }
  if (error > this->phase_max) {
    this->phase_max = error;
    this->phase_max_ms = now_ms;
  }

  if (this->phase_maxs.size() > KEPT)
    this->phase_maxs.erase(this->phase_maxs.begin());
  if (this->phase_mins.size() > KEPT)
    this->phase_mins.erase(this->phase_mins.begin());
  return phase_ended;
}

// The first phase starts wherever the room was, so it is left out: three of each.
bool PidAutotuner::AmplitudeDetector::has_enough_data() const {
  return std::min(this->phase_mins.size(), this->phase_maxs.size()) >= 3;
}

float PidAutotuner::AmplitudeDetector::mean_amplitude() const {
  float total_amplitudes = 0;
  size_t total_amplitudes_n = 0;
  for (size_t i = 1; i < std::min(this->phase_mins.size(), this->phase_maxs.size()) - 1; i++) {
    total_amplitudes += std::abs(this->phase_maxs[i] - this->phase_mins[i + 1]);
    total_amplitudes_n++;
  }
  const float mean_amplitude = total_amplitudes / total_amplitudes_n;
  // Measured from the centre: half the swing.
  return mean_amplitude / 2.0f;
}

}  // namespace esphome::climate_hub
