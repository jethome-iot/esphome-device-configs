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
    this->action_ = this->supports_heat_ && mode == HubMode::HEAT ? HubAction::HEATING : HubAction::IDLE;
  } else if (too_hot) {
    this->action_ = this->supports_cool_ && mode == HubMode::COOL ? HubAction::COOLING : HubAction::IDLE;
  } else if (this->action_ == HubAction::OFF) {
    // Nothing to hold after mode OFF or an unknown reading.
    this->action_ = HubAction::IDLE;
  }
  // Between the points the previous action stands: that latch is the hysteresis.

  return this->action_;
}

}  // namespace esphome::climate_hub
