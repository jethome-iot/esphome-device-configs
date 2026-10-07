#include "hub_climate.h"
#include <algorithm>
#include <cstring>
#include "climate_config.h"
#include "climate_hub.h"
#include "esphome/core/entity_base.h"

namespace esphome::climate_hub {

static constexpr uint32_t INTERNAL_BIT = 1u << ENTITY_FIELD_INTERNAL_SHIFT;

HubClimate::HubClimate(ClimateHub *hub, uint8_t index) : hub_(hub), index_(index) {
  // Full once, then empty: the list keeps the capacity, so no later one reallocates it.
  std::vector<const char *> all;
  for (char *buffer : this->custom_presets_)
    all.push_back(buffer);
  this->set_supported_custom_presets(all);
  this->set_supported_custom_presets(std::vector<const char *>{});
}

void HubClimate::show(const std::string &name, uint32_t entity_fields) {
  // configure_entity_ would give an empty name the device's friendly name.
  if (name.empty())
    return;
  this->rename_(name, entity_fields & ~INTERNAL_BIT);
  this->free_ = false;
}

void HubClimate::hide(uint32_t entity_fields) {
  this->configure_entity_(this->named_ ? this->names_[this->current_name_] : FREE_SLOT_NAME, 0,
                          entity_fields | INTERNAL_BIT);
  this->free_ = true;
}

void HubClimate::hide_as(const std::string &name, uint32_t entity_fields) {
  this->rename_(name, entity_fields | INTERNAL_BIT);
  this->free_ = true;
}

void HubClimate::rename_(const std::string &name, uint32_t entity_fields) {
  const uint8_t next = this->current_name_ ^ 1;
  char *buffer = this->names_[next];
  std::memset(buffer, 0, sizeof(this->names_[next]) - 1);
  std::memcpy(buffer, name.data(), std::min(name.size(), NAME_MAX_LENGTH));
  // Hash 0: derived from the name, as codegen does, so object-id-keyed records find it.
  this->configure_entity_(buffer, 0, entity_fields);
  this->current_name_ = next;
  this->named_ = true;
}

void HubClimate::park(uint32_t entity_fields) {
  this->configure_entity_(FREE_SLOT_NAME, 0, entity_fields | INTERNAL_BIT);
  this->free_ = true;
  this->named_ = false;
  this->set_traits(false, false, 5.f, 45.f, 0.5f);
  this->set_presets({});
  this->show_preset(nullptr);
}

void HubClimate::set_traits(bool heat, bool cool, float min_temperature, float max_temperature, float step) {
  this->heat_ = heat;
  this->cool_ = cool;
  this->min_temperature_ = min_temperature;
  this->max_temperature_ = max_temperature;
  this->step_ = step;
}

void HubClimate::set_presets(const std::vector<PresetConfig> &presets) {
  climate::ClimatePresetMask standard_presets;
  std::vector<const char *> custom;
  for (const PresetConfig &preset : presets) {
    climate::ClimatePreset standard;
    if (standard_preset(preset.name, &standard)) {
      standard_presets.insert(standard);
      continue;
    }
    // The document rules keep it under; the buffers are what must never overflow.
    if (custom.size() >= PRESET_MAX_COUNT)
      continue;
    // In place, the terminator untouched: a reader mid-copy sees a mix of names, never past the end.
    char *buffer = this->custom_presets_[custom.size()];
    const size_t length = std::min(preset.name.size(), NAME_MAX_LENGTH);
    std::memcpy(buffer, preset.name.data(), length);
    std::memset(buffer + length, 0, NAME_MAX_LENGTH - length);
    custom.push_back(buffer);
  }
  this->standard_presets_ = standard_presets;
  this->set_supported_custom_presets(custom);
}

void HubClimate::show_preset(const PresetConfig *preset) {
  climate::ClimatePreset standard;
  this->preset.reset();
  this->clear_custom_preset_();
  if (preset == nullptr)
    return;
  if (standard_preset(preset->name, &standard)) {
    this->set_preset_(standard);
  } else {
    this->set_custom_preset_(preset->name.c_str());
  }
}

// Built from scalars on every call; get_traits() adds the custom presets as a pointer to this
// slot's own list, so a copy owns no vector.
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
  traits.set_visual_target_temperature_step(this->step_);
  // The room to a tenth, whatever step the target moves in.
  traits.set_visual_current_temperature_step(0.1f);
  traits.set_supported_presets(this->standard_presets_);
  return traits;
}

void HubClimate::control(const climate::ClimateCall &call) {
  // A call resolved before the slot was freed must not steer whatever takes it next.
  if (this->free_)
    return;
  this->hub_->on_control_(this->index_, call);
}

}  // namespace esphome::climate_hub
