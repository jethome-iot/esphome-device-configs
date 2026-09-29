#include "hub_climate.h"
#include <algorithm>
#include <cstring>
#include "climate_config.h"
#include "climate_hub.h"
#include "esphome/core/entity_base.h"

namespace esphome::climate_hub {

static constexpr uint32_t INTERNAL_BIT = 1u << ENTITY_FIELD_INTERNAL_SHIFT;

void HubClimate::show(const std::string &name, uint32_t entity_fields) {
  // configure_entity_ would give an empty name the device's friendly name.
  if (name.empty())
    return;
  const uint8_t next = this->current_name_ ^ 1;
  char *buffer = this->names_[next];
  std::memset(buffer, 0, sizeof(this->names_[next]) - 1);
  std::memcpy(buffer, name.data(), std::min(name.size(), NAME_MAX_LENGTH));
  // Hash 0: derived from the name, as codegen does, so object-id-keyed records find it.
  this->configure_entity_(buffer, 0, entity_fields & ~INTERNAL_BIT);
  this->current_name_ = next;
  this->free_ = false;
  this->named_ = true;
}

void HubClimate::hide(uint32_t entity_fields) {
  this->configure_entity_(this->named_ ? this->names_[this->current_name_] : FREE_SLOT_NAME, 0,
                          entity_fields | INTERNAL_BIT);
  this->free_ = true;
}

void HubClimate::park(uint32_t entity_fields) {
  this->configure_entity_(FREE_SLOT_NAME, 0, entity_fields | INTERNAL_BIT);
  this->free_ = true;
  this->named_ = false;
  this->set_traits(false, false, 5.f, 45.f, 0.5f);
}

void HubClimate::set_traits(bool heat, bool cool, float min_temperature, float max_temperature, float step) {
  this->heat_ = heat;
  this->cool_ = cool;
  this->min_temperature_ = min_temperature;
  this->max_temperature_ = max_temperature;
  this->step_ = step;
}

// Built from scalars on every call: no custom modes or presets, so a copy owns no vector.
climate::ClimateTraits HubClimate::traits() {
  climate::ClimateTraits traits;
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
  if (this->heat_)
    traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  if (this->cool_)
    traits.add_supported_mode(climate::CLIMATE_MODE_COOL);
  if (this->heat_ && this->cool_)
    traits.add_supported_mode(climate::CLIMATE_MODE_HEAT_COOL);
  traits.set_visual_min_temperature(this->min_temperature_);
  traits.set_visual_max_temperature(this->max_temperature_);
  traits.set_visual_temperature_step(this->step_);
  return traits;
}

void HubClimate::control(const climate::ClimateCall &call) {
  // A call resolved before the slot was freed must not steer whatever takes it next.
  if (this->free_)
    return;
  this->hub_->on_control_(this->index_, call);
}

}  // namespace esphome::climate_hub
