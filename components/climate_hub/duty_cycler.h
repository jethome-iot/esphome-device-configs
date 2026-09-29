#pragma once

#include <cstdint>

namespace esphome::climate_hub {

/// Slow PWM: a 0..1 duty becomes a relay state over a period of minutes. Stands in for the
/// output::FloatOutput a PID wants and a relay board does not have.
class DutyCycler {
 public:
  void set_period(uint32_t period_ms) { this->period_ms_ = period_ms < 1 ? 1 : period_ms; }
  void set_duty(float duty);
  float duty() const { return this->duty_; }

  void start(uint32_t now_ms);
  void reset() { this->started_ = false; }

  /// The state the relay should be in now. Unsigned arithmetic makes the millis rollover a
  /// non-event; the phase resyncs across however many periods were missed.
  bool update(uint32_t now_ms);

 protected:
  uint32_t period_ms_{300000};
  uint32_t period_start_ms_{0};
  float duty_{0.f};
  bool started_{false};
};

}  // namespace esphome::climate_hub
