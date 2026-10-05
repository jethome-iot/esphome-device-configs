#include "controller_runtime.h"
#include <algorithm>
#include <cmath>
#include "esphome/core/log.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

namespace esphome::climate_hub {

static const char *const TAG = "climate_hub";

namespace {

climate::ClimateMode to_climate_mode(HubMode m) {
  switch (m) {
    case HubMode::HEAT:
      return climate::CLIMATE_MODE_HEAT;
    case HubMode::COOL:
      return climate::CLIMATE_MODE_COOL;
    case HubMode::HEAT_COOL:
      return climate::CLIMATE_MODE_HEAT_COOL;
    case HubMode::OFF:
    default:
      return climate::CLIMATE_MODE_OFF;
  }
}

bool from_climate_mode(climate::ClimateMode m, HubMode *out) {
  switch (m) {
    case climate::CLIMATE_MODE_OFF:
      *out = HubMode::OFF;
      return true;
    case climate::CLIMATE_MODE_HEAT:
      *out = HubMode::HEAT;
      return true;
    case climate::CLIMATE_MODE_COOL:
      *out = HubMode::COOL;
      return true;
    case climate::CLIMATE_MODE_HEAT_COOL:
      *out = HubMode::HEAT_COOL;
      return true;
    default:
      return false;
  }
}

climate::ClimateAction to_climate_action(HubAction a) {
  switch (a) {
    case HubAction::IDLE:
      return climate::CLIMATE_ACTION_IDLE;
    case HubAction::HEATING:
      return climate::CLIMATE_ACTION_HEATING;
    case HubAction::COOLING:
      return climate::CLIMATE_ACTION_COOLING;
    case HubAction::OFF:
    default:
      return climate::CLIMATE_ACTION_OFF;
  }
}

float clamp01(float v) { return std::isnan(v) ? 0.f : (v < 0.f ? 0.f : (v > 1.f ? 1.f : v)); }

}  // namespace

void ControllerRuntime::start(ClimateConfig *config, sensor::Sensor *sensor, RelayClaim *heat, RelayClaim *cool,
                              uint32_t now_ms, const Reading &last) {
  // A Save: the PWM keeps its rhythm unless apply_config_() gives it a new period.
  const bool restart = this->config_ != nullptr;
  const bool same_sensor = restart && sensor == this->sensor_;
  // What the PID learnt holds for its law and its sensor; a Save that keeps both keeps it.
  const bool keep_pid = same_sensor && this->kind_ == ControlKind::PID && config->kind == ControlKind::PID;
  // The latch too: a relay min_on holds closed after the latch let it go is not heating.
  const bool keep_latch = restart && this->kind_ == ControlKind::BANG_BANG && config->kind == ControlKind::BANG_BANG;
  this->config_ = config;
  this->sensor_ = sensor;
  this->heat_claim_ = heat;
  this->cool_claim_ = cool;
  this->kind_ = config->kind;

  if (!restart) {
    this->heat_duty_.reset();
    this->cool_duty_.reset();
  }
  if (!same_sensor)
    this->waiting_since_ms_ = now_ms;
  this->control_due_ = true;
  this->apply_config_();
  if (!keep_pid) {
    this->pid_.reset();
    this->pid_.set_starting_integral_term(config->pid.starting_integral_term);
    this->controlled_ = false;
  }

  // Inside the band the latch decides: reset, it would open a relay that is heating now. A
  // start or a take-over has no latch of its own and goes by the relays; a direction the mode
  // no longer drives is dropped, as a mode change through control() drops it.
  const HubMode mode = config->mode;
  const bool may_heat = heat != nullptr && (mode == HubMode::HEAT || mode == HubMode::HEAT_COOL);
  const bool may_cool = cool != nullptr && (mode == HubMode::COOL || mode == HubMode::HEAT_COOL);
  const HubAction latched = this->hysteresis_.action();
  const bool heating = may_heat && (keep_latch ? latched == HubAction::HEATING : heat->state());
  const bool cooling = may_cool && (keep_latch ? latched == HubAction::COOLING : cool->state());
  this->hysteresis_.seed(heating ? HubAction::HEATING : (cooling ? HubAction::COOLING : HubAction::IDLE));

  this->has_sample_ = last.seen;
  this->last_sample_ms_ = last.ms;
  this->entity_->current_temperature = last.seen ? last.value : NAN;
#ifdef USE_SENSOR
  // Shown, not acted on: a probe that fell silent long ago keeps its last state for good.
  if (!last.seen && sensor != nullptr && sensor->has_state() && std::isfinite(sensor->state))
    this->entity_->current_temperature = sensor->state;
#endif

  // The hub publishes next: with what the thermostat is doing, not "off" until the first pass.
  this->refresh_fault_(now_ms);
  this->action_ = this->standing_action_();
  this->entity_->action = to_climate_action(this->action_);
}

void ControllerRuntime::stop(uint32_t now_ms) {
  if (this->config_ == nullptr)
    return;
  this->all_relays_off_(now_ms);
  this->config_ = nullptr;
  this->sensor_ = nullptr;
  this->heat_claim_ = nullptr;
  this->cool_claim_ = nullptr;
  this->action_ = HubAction::OFF;
  this->fault_ = HubFault::NONE;
  this->has_sample_ = false;
  this->entity_->mode = climate::CLIMATE_MODE_OFF;
  this->entity_->action = climate::CLIMATE_ACTION_OFF;
  this->entity_->current_temperature = NAN;
}

void ControllerRuntime::release_claim(const RelayClaim *claim) {
  if (this->heat_claim_ == claim)
    this->heat_claim_ = nullptr;
  if (this->cool_claim_ == claim)
    this->cool_claim_ = nullptr;
}

void ControllerRuntime::apply_config_() {
  const ClimateConfig &c = *this->config_;

  this->pid_.set_gains(c.pid.kp, c.pid.ki, c.pid.kd);
  this->pid_.set_integral_limits(c.pid.min_integral, c.pid.max_integral);
  this->pid_.set_samples(static_cast<int>(c.pid.output_samples), static_cast<int>(c.pid.derivative_samples));
  this->pid_.set_deadband(c.pid.deadband_threshold_low, c.pid.deadband_threshold_high, c.pid.deadband_kp_multiplier,
                          c.pid.deadband_ki_multiplier, c.pid.deadband_kd_multiplier,
                          static_cast<int>(c.pid.deadband_output_samples));

  this->hysteresis_.set_setpoints(c.switch_low(), c.switch_high());
  this->hysteresis_.set_directions(c.supports_heat(), c.supports_cool());

  this->heat_duty_.set_period(static_cast<uint32_t>(c.heat.period_s * 1000.f));
  this->cool_duty_.set_period(static_cast<uint32_t>(c.cool.period_s * 1000.f));
  if (this->heat_claim_ != nullptr) {
    this->heat_claim_->set_dwell(static_cast<uint32_t>(c.heat.min_on_s * 1000.f),
                                 static_cast<uint32_t>(c.heat.min_off_s * 1000.f));
  }
  if (this->cool_claim_ != nullptr) {
    this->cool_claim_->set_dwell(static_cast<uint32_t>(c.cool.min_on_s * 1000.f),
                                 static_cast<uint32_t>(c.cool.min_off_s * 1000.f));
  }

  this->entity_->set_traits(c.supports_heat(), c.supports_cool(), c.visual.min_temperature, c.visual.max_temperature,
                            c.visual.step);
  this->entity_->mode = to_climate_mode(c.mode);
  // Single-point whatever the algorithm: bang-bang keeps its band in the document, so the
  // low/high pair (the same four bytes in climate::Climate) stays unwritten.
  this->entity_->target_temperature = c.setpoint;
}

bool ControllerRuntime::mode_supported_(HubMode mode) const {
  switch (mode) {
    case HubMode::OFF:
      return true;
    case HubMode::HEAT:
      return this->config_->supports_heat();
    case HubMode::COOL:
      return this->config_->supports_cool();
    case HubMode::HEAT_COOL:
      return this->config_->supports_heat() && this->config_->supports_cool();
  }
  return false;
}

bool ControllerRuntime::control(const climate::ClimateCall &call) {
  if (this->config_ == nullptr)
    return false;
  ClimateConfig &c = *this->config_;
  bool changed = false;

  HubMode requested;
  if (call.get_mode().has_value() && from_climate_mode(*call.get_mode(), &requested) &&
      this->mode_supported_(requested) && requested != c.mode) {
    // The bang-bang latch holds the last action between the switching points: carried across
    // a mode change it would keep the heater running in COOL.
    this->hysteresis_.reset();
    c.mode = requested;
    changed = true;
  }
  if (call.get_target_temperature().has_value() && !std::isnan(*call.get_target_temperature())) {
    // A client may send anything; the document only ever holds a target inside its range.
    float target = *call.get_target_temperature();
    target = std::max(c.visual.min_temperature, std::min(c.visual.max_temperature, target));
    if (target != c.setpoint) {
      c.setpoint = target;
      changed = true;
    }
  }

  this->hysteresis_.set_setpoints(c.switch_low(), c.switch_high());
  this->entity_->mode = to_climate_mode(c.mode);
  this->entity_->target_temperature = c.setpoint;
  // Published with the mode it replaced, the action would say "off" in HEAT until the next pass.
  this->set_action_(this->standing_action_());
  this->control_due_ = true;
  // Published even when nothing moved: the caller waits for the state its command produced.
  this->entity_->publish_state();
  return changed;
}

void ControllerRuntime::on_sample(float value, uint32_t now_ms) {
  if (this->config_ == nullptr)
    return;
  this->last_sample_ms_ = now_ms;
  this->has_sample_ = true;
  if (value == this->entity_->current_temperature)
    return;
  this->entity_->current_temperature = value;
  this->entity_->publish_state();
}

void ControllerRuntime::tick(uint32_t now_ms) {
  if (this->config_ == nullptr || !this->config_->enabled)
    return;
  const ClimateConfig &c = *this->config_;

  this->refresh_fault_(now_ms);
  // Waiting for a first reading is no fault, but nothing to act on either.
  if (this->fault_ != HubFault::NONE || c.mode == HubMode::OFF || !this->has_sample_) {
    this->all_relays_off_(now_ms);
    if (this->set_action_(this->standing_action_()))
      this->entity_->publish_state();
    return;
  }

  const auto interval_ms = static_cast<uint32_t>(c.update_interval_s * 1000.f);
  if (this->control_due_ || now_ms - this->last_control_ms_ >= interval_ms)
    this->run_control_(now_ms);

  this->drive_outputs_(now_ms);
}

void ControllerRuntime::refresh_fault_(uint32_t now_ms) {
  const ClimateConfig &c = *this->config_;
  const auto timeout_ms = static_cast<uint32_t>(c.safety.sensor_timeout_s * 1000.f);
  // Silence counts from the last reading, or from the start while there is none yet.
  const uint32_t silent_ms = now_ms - (this->has_sample_ ? this->last_sample_ms_ : this->waiting_since_ms_);

  HubFault fault = HubFault::NONE;
  if (silent_ms > timeout_ms) {
    fault = HubFault::SENSOR_STALE;
  } else if (this->has_sample_ && this->entity_->current_temperature > c.safety.max_temperature) {
    fault = HubFault::OVERTEMP;
  }

  if (fault == this->fault_)
    return;
  if (fault != HubFault::NONE) {
    ESP_LOGW(TAG, "'%s': %s", c.id.c_str(), enums::fault_to_string(fault));
  } else {
    ESP_LOGI(TAG, "'%s': fault cleared", c.id.c_str());
    // The fault zeroed the duties: waiting out update_interval_s would leave it off for up to an hour.
    this->control_due_ = true;
  }
  this->fault_ = fault;
}

HubAction ControllerRuntime::standing_action_() const {
  const ClimateConfig &c = *this->config_;
  if (this->fault_ != HubFault::NONE || c.mode == HubMode::OFF)
    return HubAction::OFF;
  if (!this->has_sample_)
    return HubAction::IDLE;
  if (c.kind == ControlKind::BANG_BANG)
    return this->hysteresis_.action();
  // A PID between two pulses of its PWM is still heating.
  if ((c.mode == HubMode::HEAT || c.mode == HubMode::HEAT_COOL) && this->heat_duty_.duty() > 0.f)
    return HubAction::HEATING;
  if ((c.mode == HubMode::COOL || c.mode == HubMode::HEAT_COOL) && this->cool_duty_.duty() > 0.f)
    return HubAction::COOLING;
  return HubAction::IDLE;
}

bool ControllerRuntime::set_action_(HubAction action) {
  if (action == this->action_)
    return false;
  this->action_ = action;
  this->entity_->action = to_climate_action(action);
  return true;
}

void ControllerRuntime::run_control_(uint32_t now_ms) {
  const ClimateConfig &c = *this->config_;
  const float dt_s = this->controlled_ ? static_cast<float>(now_ms - this->last_control_ms_) / 1000.f : 0.f;
  this->last_control_ms_ = now_ms;
  this->controlled_ = true;
  this->control_due_ = false;

  HubAction action;
  if (c.kind == ControlKind::PID) {
    const float output = this->pid_.update(c.setpoint, this->entity_->current_temperature, dt_s);
    const bool may_heat = c.supports_heat() && (c.mode == HubMode::HEAT || c.mode == HubMode::HEAT_COOL);
    const bool may_cool = c.supports_cool() && (c.mode == HubMode::COOL || c.mode == HubMode::HEAT_COOL);
    const float heat = may_heat ? clamp01(output) : 0.f;
    const float cool = may_cool ? clamp01(-output) : 0.f;
    this->heat_duty_.set_duty(heat);
    this->cool_duty_.set_duty(cool);
    action = heat > 0.f ? HubAction::HEATING : (cool > 0.f ? HubAction::COOLING : HubAction::IDLE);
  } else {
    action = this->hysteresis_.update(c.mode, this->entity_->current_temperature);
    this->heat_duty_.set_duty(action == HubAction::HEATING ? 1.f : 0.f);
    this->cool_duty_.set_duty(action == HubAction::COOLING ? 1.f : 0.f);
  }

  if (this->set_action_(action))
    this->entity_->publish_state();
}

void ControllerRuntime::drive_outputs_(uint32_t now_ms) {
  if (this->heat_claim_ != nullptr)
    this->heat_claim_->request(this->heat_duty_.update(now_ms), now_ms);
  if (this->cool_claim_ != nullptr)
    this->cool_claim_->request(this->cool_duty_.update(now_ms), now_ms);
}

void ControllerRuntime::all_relays_off_(uint32_t now_ms) {
  this->heat_duty_.set_duty(0.f);
  this->cool_duty_.set_duty(0.f);
  if (this->heat_claim_ != nullptr)
    this->heat_claim_->force_off(now_ms);
  if (this->cool_claim_ != nullptr)
    this->cool_claim_->force_off(now_ms);
}

float ControllerRuntime::sensor_age_s(uint32_t now_ms) const {
  if (!this->has_sample_)
    return NAN;
  return static_cast<float>(now_ms - this->last_sample_ms_) / 1000.f;
}

}  // namespace esphome::climate_hub
