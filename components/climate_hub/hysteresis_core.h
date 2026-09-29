#pragma once

#include "enums.h"

namespace esphome::climate_hub {

/// Two absolute switching points, latching in between: ESPHome's bang_bang rule with the entity
/// peeled off. Heat below `low`, cool above `high`.
class HysteresisCore {
 public:
  void set_setpoints(float low, float high);
  void set_directions(bool supports_heat, bool supports_cool);

  /// NaN anywhere reports OFF rather than IDLE: unknown is not "in range".
  HubAction update(HubMode mode, float temperature);

  void reset() { this->action_ = HubAction::OFF; }

 protected:
  float low_{0.f};
  float high_{0.f};
  bool supports_heat_{false};
  bool supports_cool_{false};
  HubAction action_{HubAction::OFF};
};

}  // namespace esphome::climate_hub
