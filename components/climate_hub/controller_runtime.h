#pragma once

#include <cstdint>
#include "climate_config.h"
#include "duty_cycler.h"
#include "enums.h"
#include "hub_climate.h"
#include "hysteresis_core.h"
#include "pid_core.h"
#include "relay_claim.h"

// A config without a sensor: section has no sensor sources; the name must still exist.
namespace esphome::sensor {
class Sensor;
}

namespace esphome::climate_hub {

/// The control loop of one running thermostat, paired for life with one entity of the pool.
/// Loop task only: the hub starts and stops it, ticks it, and hands it samples and calls.
class ControllerRuntime {
 public:
  explicit ControllerRuntime(HubClimate *entity) : entity_(entity) {}

  bool running() const { return this->config_ != nullptr; }
  HubClimate *entity() const { return this->entity_; }
  /// The running document, nullptr when stopped.
  ClimateConfig *config() const { return this->config_; }
  sensor::Sensor *sensor() const { return this->sensor_; }
  RelayClaim *heat_claim() const { return this->heat_claim_; }
  RelayClaim *cool_claim() const { return this->cool_claim_; }

  /// Runs `config` from a clean control state and puts its mode, target and traits on the
  /// entity. Also how a Save applies to a running thermostat: the claims it keeps carry their
  /// relay state and dwell over, and the last sample stands when the sensor did not change.
  void start(ClimateConfig *config, sensor::Sensor *sensor, RelayClaim *heat, RelayClaim *cool, uint32_t now_ms);
  /// Opens both relays through the claims and lets go of them and the document. The entity
  /// reads off, with no temperature.
  void stop(uint32_t now_ms);

  /// The control loop, with the clock passed in.
  void tick(uint32_t now_ms);
  /// A reading from the sensor; the hub drops NaN before it gets here. The entity is
  /// republished only when the temperature changed.
  void on_sample(float value, uint32_t now_ms);
  /// A mode or target from Home Assistant, the web server or the API. True when the document
  /// changed and needs writing.
  bool control(const climate::ClimateCall &call);

  HubAction action() const { return this->action_; }
  HubFault fault() const { return this->fault_; }
  bool has_sample() const { return this->has_sample_; }
  /// Seconds since the last reading, NaN before the first.
  float sensor_age_s(uint32_t now_ms) const;
  float heat_duty() const { return this->heat_duty_.duty(); }
  float cool_duty() const { return this->cool_duty_.duty(); }
  bool heat_relay_on() const { return this->heat_claim_ != nullptr && this->heat_claim_->state(); }
  bool cool_relay_on() const { return this->cool_claim_ != nullptr && this->cool_claim_->state(); }
  const PidCore &pid() const { return this->pid_; }

 protected:
  void apply_config_();
  void run_control_(uint32_t now_ms);
  void drive_outputs_(uint32_t now_ms);
  void all_relays_off_(uint32_t now_ms);
  bool mode_supported_(HubMode mode) const;

  HubClimate *entity_;
  ClimateConfig *config_{nullptr};
  sensor::Sensor *sensor_{nullptr};
  RelayClaim *heat_claim_{nullptr};
  RelayClaim *cool_claim_{nullptr};

  PidCore pid_;
  HysteresisCore hysteresis_;
  DutyCycler heat_duty_;
  DutyCycler cool_duty_;

  HubAction action_{HubAction::OFF};
  HubFault fault_{HubFault::NONE};
  uint32_t last_sample_ms_{0};
  uint32_t last_control_ms_{0};
  bool has_sample_{false};
  // A pass has run since start(): the next one integrates over the time since.
  bool controlled_{false};
  // Run a pass at the next tick instead of waiting out update_interval_s, up to an hour.
  bool control_due_{true};
};

}  // namespace esphome::climate_hub
