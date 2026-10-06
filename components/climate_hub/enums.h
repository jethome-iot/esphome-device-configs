#pragma once

#include <cstdint>
#include <string>

namespace esphome::climate_hub {

/// Control law. Persisted as a string, so the names are a wire format.
enum class ControlKind : uint8_t {
  PID = 0,
  BANG_BANG,
};

/// Requested operating mode: climate::ClimateMode minus what a relay pair cannot do.
enum class HubMode : uint8_t {
  OFF = 0,
  HEAT,
  COOL,
  HEAT_COOL,
};

/// What the controller is doing right now.
enum class HubAction : uint8_t {
  OFF = 0,
  IDLE,
  HEATING,
  COOLING,
};

/// Why a running controller is not controlling, or, for RELAY_CONTESTED alone, what it reports
/// while it goes on. Reported over HTTP, never persisted. A missing sensor or relay is no fault:
/// a thermostat starts only once its sensor and relays are there, and waits till then.
enum class HubFault : uint8_t {
  NONE = 0,
  SENSOR_STALE,
  OVERTEMP,
  RELAY_CONTESTED,
};

/// Where a calibration is: running, or ended with gains or without.
enum class AutotuneState : uint8_t {
  RUNNING = 0,
  SUCCEEDED,
  FAILED,
};

/// Why a calibration ended without gains. Reported over HTTP, never persisted.
enum class AutotuneEnd : uint8_t {
  NONE = 0,
  CANCELLED,
  TARGET_CHANGED,
  MODE_CHANGED,
  SAVED,
  STOPPED,
  TAKEN_OVER,
  SENSOR_STALE,
  OVERTEMP,
  RELAY_CONTESTED,
  TIMEOUT,
  NO_SWITCH,
  NOISY,
};

/// The tuning rule that turns Ku and Pu into gains. A wire format.
enum class AutotuneRule : uint8_t {
  ZN_PI = 0,
  ZN_PID,
  PESSEN,
  SOME_OVERSHOOT,
  NO_OVERSHOOT,
};

/// The relay a calibration swings: one direction per run.
enum class AutotuneDirection : uint8_t {
  HEAT = 0,
  COOL,
};

namespace enums {

const char *control_kind_to_string(ControlKind v);
bool control_kind_from_string(const std::string &s, ControlKind *out);

const char *mode_to_string(HubMode v);
bool mode_from_string(const std::string &s, HubMode *out);

const char *action_to_string(HubAction v);
const char *fault_to_string(HubFault v);

const char *autotune_state_to_string(AutotuneState v);
const char *autotune_end_to_string(AutotuneEnd v);
/// The fault a calibration ends on, NONE for none.
AutotuneEnd autotune_end_of(HubFault fault);
const char *autotune_rule_to_string(AutotuneRule v);
bool autotune_rule_from_string(const std::string &s, AutotuneRule *out);
const char *autotune_direction_to_string(AutotuneDirection v);
bool autotune_direction_from_string(const std::string &s, AutotuneDirection *out);

}  // namespace enums

}  // namespace esphome::climate_hub
