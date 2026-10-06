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
    case HubFault::SENSOR_STALE:
      return "sensor_stale";
    case HubFault::OVERTEMP:
      return "overtemp";
    case HubFault::RELAY_CONTESTED:
      return "relay_contested";
  }
  return "none";
}

const char *autotune_state_to_string(AutotuneState v) {
  switch (v) {
    case AutotuneState::RUNNING:
      return "running";
    case AutotuneState::SUCCEEDED:
      return "succeeded";
    case AutotuneState::FAILED:
      return "failed";
  }
  return "failed";
}

const char *autotune_end_to_string(AutotuneEnd v) {
  switch (v) {
    case AutotuneEnd::NONE:
      return "";
    case AutotuneEnd::CANCELLED:
      return "cancelled";
    case AutotuneEnd::TARGET_CHANGED:
      return "target_changed";
    case AutotuneEnd::MODE_CHANGED:
      return "mode_changed";
    case AutotuneEnd::SAVED:
      return "saved";
    case AutotuneEnd::STOPPED:
      return "stopped";
    case AutotuneEnd::TAKEN_OVER:
      return "taken_over";
    case AutotuneEnd::SENSOR_STALE:
      return "sensor_stale";
    case AutotuneEnd::OVERTEMP:
      return "overtemp";
    case AutotuneEnd::RELAY_CONTESTED:
      return "relay_contested";
    case AutotuneEnd::TIMEOUT:
      return "timeout";
    case AutotuneEnd::NO_SWITCH:
      return "no_switch";
    case AutotuneEnd::NOISY:
      return "noisy";
  }
  return "";
}

AutotuneEnd autotune_end_of(HubFault fault) {
  switch (fault) {
    case HubFault::NONE:
      return AutotuneEnd::NONE;
    case HubFault::SENSOR_STALE:
      return AutotuneEnd::SENSOR_STALE;
    case HubFault::OVERTEMP:
      return AutotuneEnd::OVERTEMP;
    case HubFault::RELAY_CONTESTED:
      return AutotuneEnd::RELAY_CONTESTED;
  }
  return AutotuneEnd::NONE;
}

// Upstream prints these five; the dashboard offers them under the same names.
static const struct {
  AutotuneRule rule;
  const char *name;
} RULES[] = {
    {AutotuneRule::ZN_PI, "zn_pi"},
    {AutotuneRule::ZN_PID, "zn_pid"},
    {AutotuneRule::PESSEN, "pessen"},
    {AutotuneRule::SOME_OVERSHOOT, "some_overshoot"},
    {AutotuneRule::NO_OVERSHOOT, "no_overshoot"},
};

const char *autotune_rule_to_string(AutotuneRule v) {
  for (const auto &rule : RULES) {
    if (rule.rule == v)
      return rule.name;
  }
  return "zn_pi";
}

bool autotune_rule_from_string(const std::string &s, AutotuneRule *out) {
  for (const auto &rule : RULES) {
    if (s == rule.name) {
      *out = rule.rule;
      return true;
    }
  }
  return false;
}

const char *autotune_direction_to_string(AutotuneDirection v) { return v == AutotuneDirection::COOL ? "cool" : "heat"; }

bool autotune_direction_from_string(const std::string &s, AutotuneDirection *out) {
  if (s == "heat") {
    *out = AutotuneDirection::HEAT;
    return true;
  }
  if (s == "cool") {
    *out = AutotuneDirection::COOL;
    return true;
  }
  return false;
}

}  // namespace esphome::climate_hub::enums
