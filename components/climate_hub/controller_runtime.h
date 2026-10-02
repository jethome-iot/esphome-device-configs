#pragma once

#include <cmath>
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

/// A sensor reading and when it arrived.
struct Reading {
  float value{NAN};
  uint32_t ms{0};
  bool seen{false};
};

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

  /// Runs `config` and puts its mode, target, traits and action on the entity. `last` is the
  /// latest reading the hub saw arrive from `sensor`: without one the entity shows the sensor's
  /// state, if a finite one, which may be hours old, and control waits for a reading,
  /// sensor_timeout_s from now at most. Also how a Save applies: the claims it keeps carry their
  /// relay state and dwell over, the PWM keeps its phase while its period stands, the hysteresis
  /// keeps its latch in the directions the mode still drives (a start latches on a closed relay
  /// instead), the PID keeps its state while its law and sensor stand, and so does the wait for
  /// a first reading while the sensor does.
  void start(ClimateConfig *config, sensor::Sensor *sensor, RelayClaim *heat, RelayClaim *cool, uint32_t now_ms,
             const Reading &last = Reading{});
  /// Opens both relays through the claims and lets go of them and the document. The entity
  /// reads off, with no temperature.
  void stop(uint32_t now_ms);
  /// Lets go of `claim` without touching its relay: another thermostat carries on with it.
  void release_claim(const RelayClaim *claim);

  /// The control loop, with the clock passed in.
  void tick(uint32_t now_ms);
  /// A reading from the sensor; the hub drops NaN and infinities before they get here. The
  /// entity is republished only when the temperature changed.
  void on_sample(float value, uint32_t now_ms);
  /// A mode or target from Home Assistant, the web server or the API. True when the document
  /// changed and needs writing.
  bool control(const climate::ClimateCall &call);

  /// OFF only in mode off, on a fault or stopped; otherwise IDLE when neither heating nor cooling.
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
  /// Sets fault_ from the sensor, the reading and the claims; logs a change.
  void refresh_fault_(uint32_t now_ms);
  /// What the thermostat is doing until its next pass: the latch, or the PID's duties as the
  /// mode lets them run.
  HubAction standing_action_() const;
  /// Puts `action` on the entity; true when it changed.
  bool set_action_(HubAction action);
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
  // The law the PID state was built under: on a Save, config_ already holds the new one.
  ControlKind kind_{ControlKind::PID};
  uint32_t last_sample_ms_{0};
  // Since when the sensor has had its chance to give a first reading.
  uint32_t waiting_since_ms_{0};
  uint32_t last_control_ms_{0};
  bool has_sample_{false};
  // A pass has run since start(): the next one integrates over the time since.
  bool controlled_{false};
  // Run a pass at the next tick instead of waiting out update_interval_s, up to an hour.
  bool control_due_{true};
};

}  // namespace esphome::climate_hub
