#pragma once

#include "esphome/core/component.h"

namespace esphome::config_base {

// One settings type (a file or an NVS namespace) managed by a keeper.
class SettingsBaseCommon {
 public:
  virtual ~SettingsBaseCommon() = default;

  bool is_dirty() const { return this->dirty_; }
  void mark_dirty() { this->dirty_ = true; }
  void clear_dirty() { this->dirty_ = false; }

  // Push the loaded values into the entities/components; runs at TSettings::APPLY_PRIORITY.
  virtual void apply() = 0;
  virtual size_t size() = 0;
  virtual void reset() = 0;
  // Before load; a place to capture compiled defaults or register callbacks.
  virtual void init() {}

 protected:
  bool dirty_{false};
};

// Calls settings->apply() once, at the priority the settings type declares. Registered from
// codegen, so it sorts with every other component instead of being appended during setup.
template<typename TSettings> class SettingsApplyComponent : public Component {
 public:
  explicit SettingsApplyComponent(TSettings *settings) : settings_(settings) {}
  float get_setup_priority() const override { return TSettings::APPLY_PRIORITY; }
  void setup() override {
    if (this->settings_ != nullptr)
      this->settings_->apply();
  }

 protected:
  TSettings *settings_;
};

}  // namespace esphome::config_base
