#include "config_nvs.h"
#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::config_nvs {

ConfigNvsKeeper *global_config_nvs_keeper = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

ConfigNvsKeeper::ConfigNvsKeeper() { global_config_nvs_keeper = this; }

void ConfigNvsKeeper::setup() { this->setup_common_(); }

void ConfigNvsKeeper::dump_config() {
  ESP_LOGCONFIG(TAG, "Config NVS:");
  ESP_LOGCONFIG(TAG, "  Save delay: %u ms", static_cast<unsigned>(this->save_delay_ms_));
  for (auto *settings : this->settings_list_) {
    if (settings != nullptr)
      ESP_LOGCONFIG(TAG, "  %s: %u records", settings->get_namespace(), static_cast<unsigned>(settings->size()));
  }
}

}  // namespace esphome::config_nvs

#endif  // USE_ESP32
