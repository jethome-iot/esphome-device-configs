#include "hysteresis_core.h"
#include <cmath>

namespace esphome::climate_hub {

void HysteresisCore::set_setpoints(float low, float high) {
  this->low_ = low;
  this->high_ = high;
}

void HysteresisCore::set_directions(bool supports_heat, bool supports_cool) {
  this->supports_heat_ = supports_heat;
  this->supports_cool_ = supports_cool;
}

HubAction HysteresisCore::update(HubMode mode, float temperature) {
  if (mode == HubMode::OFF) {
    this->action_ = HubAction::OFF;
    return this->action_;
  }
  if (std::isnan(temperature) || std::isnan(this->low_) || std::isnan(this->high_)) {
    this->action_ = HubAction::OFF;
    return this->action_;
  }

  const bool too_cold = temperature < this->low_;
  const bool too_hot = temperature > this->high_;

  if (too_cold) {
    const bool may_heat = this->supports_heat_ && (mode == HubMode::HEAT || mode == HubMode::HEAT_COOL);
    this->action_ = may_heat ? HubAction::HEATING : HubAction::IDLE;
  } else if (too_hot) {
    const bool may_cool = this->supports_cool_ && (mode == HubMode::COOL || mode == HubMode::HEAT_COOL);
    this->action_ = may_cool ? HubAction::COOLING : HubAction::IDLE;
  } else if ((this->supports_heat_ && this->supports_cool_ && mode == HubMode::HEAT_COOL) ||
             this->action_ == HubAction::OFF) {
    // Both directions live, or nothing to hold after mode OFF or an unknown reading.
    this->action_ = HubAction::IDLE;
  }
  // Between the points in a single-direction mode the previous action stands: that latch is
  // the hysteresis.

  return this->action_;
}

}  // namespace esphome::climate_hub
