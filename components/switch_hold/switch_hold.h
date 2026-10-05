#pragma once

#include <string>
#include <utility>
#include "esphome/core/helpers.h"

// A config without a switch: section has no switch sources; the name must still exist.
namespace esphome::switch_ {
class Switch;
}

namespace esphome::switch_hold {

/// A component that drives some switches itself and must not have them moved behind its back:
/// climate_hub's running thermostats.
class SwitchHolder {
 public:
  /// The name of whoever holds `sw`, "" when nobody does.
  virtual std::string holder_of(const switch_::Switch *sw) const = 0;

 protected:
  ~SwitchHolder() = default;
};

namespace detail {
struct Registry {
  SwitchHolder *holder{nullptr};
  CallbackManager<void(switch_::Switch *)> on_release;
};
inline Registry &registry() {
  static Registry instance;
  return instance;
}
}  // namespace detail

// Everything below runs on the loop task, as every writer of a switch does.

/// The one holder this firmware has; a later call replaces it.
inline void set_holder(SwitchHolder *holder) { detail::registry().holder = holder; }

/// The name of whoever holds `sw`, "" when nobody does or nothing is registered.
inline std::string holder(const switch_::Switch *sw) {
  SwitchHolder *holder = detail::registry().holder;
  return holder == nullptr || sw == nullptr ? std::string() : holder->holder_of(sw);
}

/// `callback(sw)` runs whenever a holder lets go of `sw`, once nothing holds it.
template<typename F> void add_on_release_callback(F &&callback) {
  detail::registry().on_release.add(std::forward<F>(callback));
}

/// From the holder, once it has let go of `sw` and nothing holds it.
inline void notify_released(switch_::Switch *sw) { detail::registry().on_release.call(sw); }

}  // namespace esphome::switch_hold
