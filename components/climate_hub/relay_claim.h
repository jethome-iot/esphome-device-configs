#pragma once

#include <cstdint>
#include <string>
#include "esphome/core/defines.h"

// A config without a switch: section has no switch sources; the name must still exist.
namespace esphome::switch_ {
class Switch;
}

namespace esphome::climate_hub {

/// Where a relay last moved to, and when.
struct RelaySwitching {
  bool on{false};
  uint32_t ms{0};
};

/// Exclusive hold on one relay, with a minimum on and off dwell. The only place in the
/// component that switches a relay, so contact wear has a single owner.
class RelayClaim {
 public:
  RelayClaim(switch_::Switch *sw, std::string owner_id) : sw_(sw), owner_(std::move(owner_id)) {}

  void set_dwell(uint32_t min_on_ms, uint32_t min_off_ms);

  /// Returns what the relay is left at, which lags `want` while a dwell floor still runs.
  bool request(bool want, uint32_t now_ms);

  /// Safety and teardown path: opens the relay regardless of the min-on floor, and also when
  /// the claim already believes it open but something else closed it.
  void force_off(uint32_t now_ms);

  /// Carries on from a switching an earlier claim on the relay made, dwell and all.
  void resume(const RelaySwitching &last);
  /// The last switching this claim made or resumed; false before there is one.
  bool last_switching(RelaySwitching *out) const;

  bool state() const { return this->state_; }
  const std::string &owner() const { return this->owner_; }
  void set_owner(const std::string &owner) { this->owner_ = owner; }
  switch_::Switch *relay() const { return this->sw_; }

 protected:
  void apply_(bool on, uint32_t now_ms);
  /// The switch's own state; the claim's belief when there is no switch.
  bool relay_state_() const;

  switch_::Switch *sw_;
  std::string owner_;
  uint32_t min_on_ms_{0};
  uint32_t min_off_ms_{0};
  uint32_t last_change_ms_{0};
  bool state_{false};
  bool initialized_{false};
};

}  // namespace esphome::climate_hub
