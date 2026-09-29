#pragma once

#include <cstddef>
#include <vector>

namespace esphome::climate_hub {

/// The newest few values of a series, for a moving average. Storage is taken when a window is
/// first sized and kept after: every slot of the pool has a controller from boot, and most of
/// them never run.
class SampleWindow {
 public:
  /// Room for `samples` values, keeping those held; never shrinks.
  void reserve(size_t samples);
  /// Adds `value`, keeps the newest `samples` and returns their mean.
  float push_average(float value, size_t samples);
  void clear() { this->count_ = 0; }

 protected:
  // A ring as long as the vector, the oldest value at head_.
  std::vector<float> values_;
  size_t head_{0};
  size_t count_{0};
};

/// ESPHome's pid control law with the clock lifted out: update() takes dt in seconds, so the
/// loop is reproducible on the host. Term for term the same as components/pid/pid_controller,
/// whose fields are private to PIDClimate and whose dt comes from millis().
class PidCore {
 public:
  void set_gains(float kp, float ki, float kd);
  void set_integral_limits(float min_integral, float max_integral);
  void set_samples(int output_samples, int derivative_samples);
  void set_deadband(float threshold_low, float threshold_high, float kp_multiplier, float ki_multiplier,
                    float kd_multiplier, int output_samples);
  void set_starting_integral_term(float v) { this->accumulated_integral_ = v; }

  /// The control effort, positive to heat and negative to cool.
  float update(float setpoint, float process_value, float dt_s);

  /// Drops the integral and both smoothing windows; leaves the tuning alone.
  void reset();

  bool in_deadband() const;

  float error() const { return this->error_; }
  float proportional_term() const { return this->proportional_term_; }
  float integral_term() const { return this->integral_term_; }
  float derivative_term() const { return this->derivative_term_; }

 protected:
  void calculate_proportional_term_();
  void calculate_integral_term_(float dt_s);
  void calculate_derivative_term_(float setpoint, float dt_s);
  void reserve_windows_();

  float kp_{0.f};
  float ki_{0.f};
  float kd_{0.f};
  float min_integral_{-1.f};
  float max_integral_{1.f};
  int output_samples_{1};
  int derivative_samples_{8};

  float threshold_low_{0.f};
  float threshold_high_{0.f};
  float kp_multiplier_{0.f};
  float ki_multiplier_{0.f};
  float kd_multiplier_{0.f};
  int deadband_output_samples_{1};

  float error_{0.f};
  float proportional_term_{0.f};
  float integral_term_{0.f};
  float derivative_term_{0.f};

  float previous_error_{0.f};
  float previous_setpoint_{0.f};
  bool has_previous_setpoint_{false};
  float accumulated_integral_{0.f};

  SampleWindow derivative_window_;
  // Shared by the output averaging inside and outside the deadband, as upstream does.
  SampleWindow output_window_;
};

}  // namespace esphome::climate_hub
