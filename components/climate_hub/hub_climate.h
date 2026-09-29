#pragma once

#include <cstdint>
#include <string>
#include "esphome/components/climate/climate.h"

namespace esphome::climate_hub {

class ClimateHub;

/// The name of a slot no thermostat has had since boot. '/' is the one character no web_server
/// URL segment can carry, so nothing can address it; an empty name would fall back to the device's.
/// As long as a name buffer and zero-filled, so a reader that pairs this pointer with a longer
/// name's length stays inside it.
inline constexpr char FREE_SLOT_NAME[64] = "climate_hub/free";

/// One climate entity of the hub's pool. The hub registers every slot with App in its setup()
/// and never frees one: a thermostat that starts takes a free slot, one that stops hands it
/// back, hidden again under the name it had.
///
/// Invariant: the name and the traits change only in setup() or inside an HTTP-originated
/// loop job, while the single HTTP task is parked in run_on_loop(). The web server reads both
/// from that task (the name to match a URL, traits() for a climate's JSON); the two name
/// buffers and the scalar-built traits keep even a torn read in bounds. Nothing here points at
/// a document.
class HubClimate final : public climate::Climate {
 public:
  HubClimate(ClimateHub *hub, uint8_t index) : hub_(hub), index_(index) {}

  uint8_t index() const { return this->index_; }
  bool is_free() const { return this->free_; }
  /// Whether the slot carries a thermostat's name rather than the placeholder.
  bool is_named() const { return this->named_; }

  /// Visible under `name`, which the caller has validated; its object id derives from it.
  void show(const std::string &name, uint32_t entity_fields);
  /// Internal, keeping its name, key and traits: an API client may still be encoding the
  /// entity it listed a moment ago, and upstream reads the name then, not when it queued it.
  void hide(uint32_t entity_fields);
  /// A hidden slot back under the placeholder, for when its name is about to be someone else's.
  void park(uint32_t entity_fields);
  void set_traits(bool heat, bool cool, float min_temperature, float max_temperature, float step);

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  ClimateHub *hub_;
  uint8_t index_;
  bool free_{true};
  bool named_{false};
  // EntityBase keeps a StringRef to the name, not a copy. show() alternates the buffers, so a
  // reader still holding the previous pointer reads intact bytes; byte 63 is never written.
  char names_[2][64]{};
  uint8_t current_name_{0};
  bool heat_{false};
  bool cool_{false};
  float min_temperature_{5.f};
  float max_temperature_{45.f};
  float step_{0.5f};
};

}  // namespace esphome::climate_hub
