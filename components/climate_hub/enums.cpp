#include "enums.h"

namespace esphome::climate_hub::enums {

const char *control_kind_to_string(ControlKind v) {
  switch (v) {
    case ControlKind::PID:
      return "pid";
    case ControlKind::BANG_BANG:
      return "bang_bang";
  }
  return "pid";
}

bool control_kind_from_string(const std::string &s, ControlKind *out) {
  if (s == "pid") {
    *out = ControlKind::PID;
    return true;
  }
  if (s == "bang_bang") {
    *out = ControlKind::BANG_BANG;
    return true;
  }
  return false;
}

const char *mode_to_string(HubMode v) {
  switch (v) {
    case HubMode::OFF:
      return "off";
    case HubMode::HEAT:
      return "heat";
    case HubMode::COOL:
      return "cool";
    case HubMode::HEAT_COOL:
      return "heat_cool";
  }
  return "off";
}

bool mode_from_string(const std::string &s, HubMode *out) {
  if (s == "off") {
    *out = HubMode::OFF;
    return true;
  }
  if (s == "heat") {
    *out = HubMode::HEAT;
    return true;
  }
  if (s == "cool") {
    *out = HubMode::COOL;
    return true;
  }
  if (s == "heat_cool") {
    *out = HubMode::HEAT_COOL;
    return true;
  }
  return false;
}

const char *action_to_string(HubAction v) {
  switch (v) {
    case HubAction::OFF:
      return "off";
    case HubAction::IDLE:
      return "idle";
    case HubAction::HEATING:
      return "heating";
    case HubAction::COOLING:
      return "cooling";
  }
  return "off";
}

const char *fault_to_string(HubFault v) {
  switch (v) {
    case HubFault::NONE:
      return "none";
    case HubFault::SENSOR_MISSING:
      return "sensor_missing";
    case HubFault::SENSOR_STALE:
      return "sensor_stale";
    case HubFault::RELAY_MISSING:
      return "relay_missing";
    case HubFault::OVERTEMP:
      return "overtemp";
  }
  return "none";
}

}  // namespace esphome::climate_hub::enums
