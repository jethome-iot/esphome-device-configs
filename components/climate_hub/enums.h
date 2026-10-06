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

/// What holds a relay away from where its thermostat wants it: the relay's own minimum on time
/// before it may open, or minimum off time before it may close. Reported over HTTP, never
/// persisted.
enum class RelayWait : uint8_t {
  NONE = 0,
  MIN_ON,
  MIN_OFF,
};

namespace enums {

const char *control_kind_to_string(ControlKind v);
bool control_kind_from_string(const std::string &s, ControlKind *out);

const char *mode_to_string(HubMode v);
bool mode_from_string(const std::string &s, HubMode *out);

const char *action_to_string(HubAction v);
const char *fault_to_string(HubFault v);
const char *relay_wait_to_string(RelayWait v);

}  // namespace enums

}  // namespace esphome::climate_hub
