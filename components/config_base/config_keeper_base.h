#pragma once

#include <cstring>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "settings_base_common.h"

namespace esphome::config_base {

// CRTP keeper: owns a list of settings, loads them at setup, saves the dirty ones after a
// debounce. TDerived provides get_timeout_name(), get_log_tag(), get_settings_key(),
// load_one() and save_one().
template<typename TDerived, typename TSettings> class ConfigKeeperBase : public Component {
 public:
  static_assert(std::is_base_of<SettingsBaseCommon, TSettings>::value,
                "TSettings must inherit from SettingsBaseCommon");

  ConfigKeeperBase() = default;

  // Before every entity's setup, so the apply components can be registered ahead of them.
  float get_setup_priority() const override { return setup_priority::HARDWARE + 5.0f; }

  void set_save_delay(uint32_t delay_ms) { this->save_delay_ms_ = delay_ms; }
  uint32_t get_save_delay() const { return this->save_delay_ms_; }

  void add_settings(TSettings *settings) {
    if (settings != nullptr)
      this->settings_list_.push_back(settings);
  }

  // False when nothing written from here would reach flash: setup() failed, so there is no
  // filesystem under the keeper and the scheduler drops a failed component's timeouts. A
  // caller that can report a failure asks first, before it changes anything in RAM.
  bool can_save() const { return !this->is_failed(); }

  // Debounced: several calls inside the delay end in one write.
  void save() {
    auto *derived = static_cast<TDerived *>(this);
    if (!this->can_save()) {
      ESP_LOGE(derived->get_log_tag(), "Storage unavailable: the change lasts until the next reboot only");
      return;
    }
    this->cancel_timeout(derived->get_timeout_name());
    this->save_pending_ = true;
    this->set_timeout(derived->get_timeout_name(), this->save_delay_ms_, [this]() {
      this->save_pending_ = false;
      this->save_all_();
    });
    ESP_LOGD(derived->get_log_tag(), "Save scheduled in %u ms", static_cast<unsigned>(this->save_delay_ms_));
  }

  void save_immediate() {
    this->cancel_pending_save();
    this->save_all_();
  }

  void save(const char *key) {
    auto *derived = static_cast<TDerived *>(this);
    if (this->get_settings(key) == nullptr) {
      ESP_LOGW(derived->get_log_tag(), "Settings '%s' not found", key);
      return;
    }
    this->save();
  }

  void save_immediate(const char *key) {
    this->cancel_pending_save();
    this->save_one_(key);
    for (auto *settings : this->settings_list_) {
      if (settings != nullptr && settings->is_dirty()) {
        this->save();
        break;
      }
    }
  }

  // Unconditional: a write that failed earlier left its type dirty with no timer pending, and
  // save_all_() skips a clean state anyway.
  void on_shutdown() override { this->save_immediate(); }

  void cancel_pending_save() {
    if (this->save_pending_) {
      auto *derived = static_cast<TDerived *>(this);
      this->cancel_timeout(derived->get_timeout_name());
      this->save_pending_ = false;
      ESP_LOGD(derived->get_log_tag(), "Pending save cancelled");
    }
  }

  bool is_save_pending() const { return this->save_pending_; }

  TSettings *get_settings(const char *key) {
    auto *derived = static_cast<TDerived *>(this);
    for (auto *settings : this->settings_list_) {
      if (settings != nullptr && strcmp(derived->get_settings_key(settings), key) == 0)
        return settings;
    }
    return nullptr;
  }

  const std::vector<TSettings *> &settings() const { return this->settings_list_; }

  void reset_all() {
    for (auto *settings : this->settings_list_) {
      if (settings != nullptr)
        settings->reset();
    }
    this->save_immediate();
    ESP_LOGI(static_cast<TDerived *>(this)->get_log_tag(), "Reset all settings");
  }

 protected:
  void setup_common_() {
    auto *derived = static_cast<TDerived *>(this);
    for (auto *settings : this->settings_list_) {
      if (settings != nullptr)
        settings->init();
    }
    for (auto *settings : this->settings_list_) {
      if (settings != nullptr)
        derived->load_one(settings);
    }
  }

  void save_all_() {
    auto *derived = static_cast<TDerived *>(this);
    // on_shutdown() comes through here at every reboot, and save() has already said its piece:
    // without a mount this would only mkdir and fopen into nothing.
    if (!this->can_save())
      return;
    bool has_dirty = false;
    for (auto *settings : this->settings_list_) {
      if (settings != nullptr && settings->is_dirty()) {
        has_dirty = true;
        break;
      }
    }
    if (!has_dirty) {
      ESP_LOGD(derived->get_log_tag(), "No settings modified, skipping save");
      return;
    }
    for (auto *settings : this->settings_list_) {
      if (settings != nullptr && settings->is_dirty())
        derived->save_one(settings);
    }
  }

  void save_one_(const char *key) {
    auto *derived = static_cast<TDerived *>(this);
    if (!this->can_save())
      return;
    TSettings *settings = this->get_settings(key);
    if (settings == nullptr) {
      ESP_LOGW(derived->get_log_tag(), "Settings '%s' not found", key);
      return;
    }
    if (!settings->is_dirty()) {
      ESP_LOGD(derived->get_log_tag(), "Settings '%s' not modified, skipping save", key);
      return;
    }
    derived->save_one(settings);
  }

  std::vector<TSettings *> settings_list_;
  uint32_t save_delay_ms_{1000};
  bool save_pending_{false};
};

}  // namespace esphome::config_base
