#pragma once

#include <cstdint>

namespace esphome::climate_hub {

/// Slow PWM: a 0..1 duty becomes a relay state over a period of minutes. Stands in for the
/// output::FloatOutput a PID wants and a relay board does not have.
class DutyCycler {
 public:
  /// A new period starts a new phase; the same one keeps the rhythm it has.
  void set_period(uint32_t period_ms);
  void set_duty(float duty);
  float duty() const { return this->duty_; }

  void start(uint64_t now_ms);
  void reset() { this->started_ = false; }

  /// The state the relay should be in now; the phase resyncs across however many periods were
  /// missed.
  bool update(uint64_t now_ms);

 protected:
  uint32_t period_ms_{300000};
  uint64_t period_start_ms_{0};
  float duty_{0.f};
  bool started_{false};
};

}  // namespace esphome::climate_hub
