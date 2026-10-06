#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace esphome::climate_hub {

/// ESPHome's relay-oscillation autotuner (components/pid/pid_autotuner) with the clock passed in
/// and every member initialised: the same relay function, the same two detectors, the same stop
/// point and the same Ku and Pu, so a run gives upstream's numbers. Added beside them: the
/// extremes in the order they came, for a chart and for the spread of the swings, and a cap on
/// the crossings it keeps.
class PidAutotuner {
 public:
  struct Gains {
    float kp;
    float ki;
    float kd;
  };
  /// The error at the far end of one relay phase, and when it got there.
  struct Extreme {
    uint32_t ms;
    float error;
  };

  /// Narrows the relay's outputs: heating alone is (0, 1), cooling alone (-1, 0).
  void config(float output_min, float output_max);
  /// The relay switches at ±noiseband; the zero-crossing detector at a quarter of it.
  void set_noiseband(float noiseband);

  /// One sample: the relay's output for it, `output_positive` or `output_negative`. Once
  /// finished() it changes nothing and returns 0.
  float update(float setpoint, float process_value, uint32_t now_ms);
  bool finished() const { return this->finished_; }

  /// The ultimate gain and period, in seconds; 0 until finished().
  float ku() const { return this->ku_; }
  float pu() const { return this->pu_; }
  /// Upstream's calculate_pid_(): kp = f·Ku, ki = f·Ku/Pu, kd = f·Ku·Pu.
  Gains gains(float kp_factor, float ki_factor, float kd_factor) const;

  /// Upstream's check: the shortest half-period at least 0.66 of the longest.
  bool symmetrical() const { return this->frequency_.symmetrical(); }
  /// The smallest peak-to-peak swing over the largest, the first phase left out; NaN with
  /// fewer than two swings.
  float swing_ratio() const;

  /// Crossings of the target it keeps the time between: a run stops at a handful, a probe that
  /// hovers at the target crosses it reading after reading.
  static constexpr size_t MAX_INTERVALS = 64;
  /// More intervals came than it keeps: the readings are noise around the target, not a swing, and
  /// the run neither keeps them nor finishes.
  bool noisy() const { return this->frequency_.overflowed; }

  /// Relay switches so far.
  uint32_t phase_count() const { return this->relay_.phase_count; }
  /// Whether a sample has set the relay yet.
  bool started() const { return this->relay_.state != RelayFunction::INIT; }
  /// Whether the relay gives `output_positive` now.
  bool positive() const { return this->relay_.state == RelayFunction::POSITIVE; }
  /// The error the next switch waits for: -noiseband on the positive side, +noiseband on the
  /// negative, 0 before the first sample.
  float target_error() const { return this->relay_.current_target_error(); }
  /// One per finished relay phase, the first phase's too.
  const std::vector<Extreme> &extremes() const { return this->extremes_; }

 protected:
  struct RelayFunction {
    enum State : uint8_t { INIT, POSITIVE, NEGATIVE };

    float update(float error);
    float current_target_error() const;

    State state{INIT};
    float noiseband{0.5f};
    float output_positive{1.f};
    float output_negative{-1.f};
    uint32_t phase_count{0};
  };

  struct FrequencyDetector {
    enum State : uint8_t { INIT, POSITIVE, NEGATIVE };

    void update(uint32_t now_ms, float error);
    bool has_enough_data() const { return this->intervals.size() >= 2; }
    float mean_period_s() const;
    bool symmetrical() const;

    State state{INIT};
    float noiseband{0.05f};
    // Upstream takes a crossing at 0 ms for none yet; the flag tells them apart.
    bool crossed{false};
    bool overflowed{false};
    uint32_t last_crossing_ms{0};
    std::vector<uint32_t> intervals;
  };

  struct AmplitudeDetector {
    /// The extreme of the phase `relay_state` ends, when it ends one; true then.
    bool update(float error, RelayFunction::State relay_state, uint32_t now_ms, Extreme *ended);
    bool has_enough_data() const;
    float mean_amplitude() const;

    float phase_min{NAN};
    float phase_max{NAN};
    uint32_t phase_min_ms{0};
    uint32_t phase_max_ms{0};
    std::vector<float> phase_mins;
    std::vector<float> phase_maxs;
    RelayFunction::State last_relay_state{RelayFunction::INIT};
  };

  RelayFunction relay_;
  FrequencyDetector frequency_;
  AmplitudeDetector amplitude_;
  std::vector<Extreme> extremes_;
  float ku_{0.f};
  float pu_{0.f};
  bool finished_{false};
};

}  // namespace esphome::climate_hub
