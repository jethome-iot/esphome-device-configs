#pragma once

#include "esphome/core/defines.h"
#ifdef JXD_CONFIG_TIMEZONE

#include <string>
#include <vector>
#include "esphome/components/config_json/settings_base_json.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/log.h"

namespace esphome::jxd_config {

// {"version": 1, "timezone": "Europe/Moscow"}, pushed to every registered time component.
class TimezoneSettingsJson : public config_json::SettingsBaseJson {
 public:
  static constexpr const char *TAG = "jxd_config.timezone";
  static constexpr const char *NAME = "timezone";

  const char *get_key() override { return NAME; }

  void set_timezone(const std::string &timezone) {
    if (this->timezone_ != timezone) {
      this->timezone_ = timezone;
      this->mark_dirty();
    }
  }
  const std::string &get_timezone() const { return this->timezone_; }

  void add_time_component(time::RealTimeClock *time_component) {
    if (time_component != nullptr)
      this->time_components_.push_back(time_component);
  }

  bool parse_json(JsonObject root, uint32_t version) override {
    if (root["timezone"].is<const char *>()) {
      const char *tz = root["timezone"];
      if (tz == nullptr) {
        ESP_LOGE(TAG, "Timezone field is null");
        return false;
      }
      this->timezone_ = tz;
      this->loaded_ = true;
    }
    return true;
  }

  void write_json(JsonObject root, uint32_t version) override { root["timezone"] = this->timezone_; }

  // Without a stored value the compiled timezone stays.
  void apply() override {
    if (!this->loaded_)
      return;
    for (auto *time_comp : this->time_components_) {
      time_comp->set_timezone(this->timezone_);
      ESP_LOGD(TAG, "Applied timezone '%s'", this->timezone_.c_str());
    }
  }

  static constexpr float APPLY_PRIORITY = setup_priority::HARDWARE + 1.0f;

  size_t size() override { return this->timezone_.empty() ? 0 : 1; }

  void reset() override {
    this->timezone_ = "GMT";
    this->loaded_ = true;
    this->mark_dirty();
    ESP_LOGI(TAG, "Reset timezone to GMT");
  }

 protected:
  std::string timezone_{"GMT"};
  bool loaded_{false};
  std::vector<time::RealTimeClock *> time_components_;
};

}  // namespace esphome::jxd_config

#endif  // JXD_CONFIG_TIMEZONE
