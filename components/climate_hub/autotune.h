#pragma once

#include <cstdint>
#include "enums.h"
#include "pid_autotuner.h"

namespace esphome::climate_hub {

/// The relay closes this far below the target and opens this far above it (heating; cooling
/// the other way round): upstream's default noiseband.
static constexpr float AUTOTUNE_NOISEBAND = 0.25f;
/// A run ends after this long, whatever it has measured.
static constexpr uint32_t AUTOTUNE_MAX_MS = 24u * 3600u * 1000u;
/// And after this long without a relay switch: the relay cannot move the room across the band.
static constexpr uint32_t AUTOTUNE_STALL_MS = 6u * 3600u * 1000u;
/// A run whose smallest swing is under this share of its largest is flagged uneven: the ratio
/// upstream's symmetry check holds the half-periods to.
static constexpr float AUTOTUNE_EVEN_RATIO = 0.66f;

struct PidGains {
  float kp{0.f};
  float ki{0.f};
  float kd{0.f};
};

/// One calibration of one thermostat. The hub keeps it from its start until the next start, a
/// delete or a reboot; the thermostat's ControllerRuntime feeds it while it runs. Loop task only.
class AutotuneRun {
 public:
  AutotuneRun(AutotuneDirection direction, AutotuneRule rule, const PidGains &gains, float setpoint, uint32_t now_ms);

  /// One sample: whether the run's relay should be closed now.
  bool feed(float value, uint32_t now_ms);
  /// The limit the run has reached at `now_ms`, NONE within both.
  AutotuneEnd limit_reached(uint32_t now_ms) const;
  /// The tuner has measured Ku and Pu; the hub then stores result() and calls succeed().
  bool found() const { return this->tuner_.finished(); }
  /// The rule's gains, each clamped into the parameter table; `clamped` says whether one had to be.
  PidGains result(bool *clamped) const;
  void succeed(const PidGains &gains, bool clamped, bool persisted, uint32_t now_ms);
  /// The thermostat's file was written since: the gains are in it, or whatever replaced them.
  void mark_persisted() { this->persisted_ = true; }
  void fail(AutotuneEnd why, uint32_t now_ms);

  AutotuneState state() const { return this->state_; }
  bool running() const { return this->state_ == AutotuneState::RUNNING; }
  AutotuneEnd reason() const { return this->reason_; }
  AutotuneDirection direction() const { return this->direction_; }
  AutotuneRule rule() const { return this->rule_; }
  /// The target it swings the room around, the thermostat's when it started: a later one ends it.
  float setpoint() const { return this->setpoint_; }
  /// Since the start, up to the end once it ended.
  uint32_t elapsed_ms(uint32_t now_ms) const;
  uint32_t started_ms() const { return this->started_ms_; }
  const PidAutotuner &tuner() const { return this->tuner_; }
  /// The gains in force when it started, and what it wrote in their place on success.
  const PidGains &old_gains() const { return this->old_gains_; }
  const PidGains &new_gains() const { return this->new_gains_; }
  /// The quality flags, set on success: they warn and never extend a run.
  bool asymmetric() const { return this->asymmetric_; }
  bool uneven() const { return this->uneven_; }
  bool clamped() const { return this->clamped_; }
  /// False when the gains run but did not reach the file, until the next write of it.
  bool persisted() const { return this->persisted_; }

 protected:
  PidAutotuner tuner_;
  AutotuneDirection direction_;
  AutotuneRule rule_;
  AutotuneState state_{AutotuneState::RUNNING};
  AutotuneEnd reason_{AutotuneEnd::NONE};
  PidGains old_gains_;
  PidGains new_gains_;
  float setpoint_;
  uint32_t started_ms_;
  uint32_t ended_ms_{0};
  uint32_t last_switch_ms_;
  uint32_t switches_{0};
  bool asymmetric_{false};
  bool uneven_{false};
  bool clamped_{false};
  bool persisted_{true};
};

}  // namespace esphome::climate_hub
