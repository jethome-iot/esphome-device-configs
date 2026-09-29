#pragma once

#include <deque>

namespace esphome::climate_hub {

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
  static float weighted_average(std::deque<float> &list, float new_value, int samples);

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

  std::deque<float> derivative_list_;
  std::deque<float> output_list_;
};

}  // namespace esphome::climate_hub
