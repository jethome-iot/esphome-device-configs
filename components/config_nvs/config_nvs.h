#pragma once

#include "esphome/core/defines.h"
#ifdef USE_ESP32

#include "esphome/components/config_base/config_keeper_base.h"
#include "esphome/core/component.h"
#include "settings_base_nvs.h"

namespace esphome::config_nvs {

static const char *const TAG = "config_nvs";

class ConfigNvsKeeper : public config_base::ConfigKeeperBase<ConfigNvsKeeper, SettingsBaseNvs> {
 public:
  ConfigNvsKeeper();

  void setup() override;
  void dump_config() override;

  const char *get_timeout_name() const { return "nvs_save"; }
  const char *get_log_tag() const { return TAG; }
  const char *get_settings_key(SettingsBaseNvs *settings) const { return settings->get_namespace(); }
  void load_one(SettingsBaseNvs *settings) { settings->load_from_nvs(); }
  void save_one(SettingsBaseNvs *settings) { settings->save_to_nvs(); }
};

extern ConfigNvsKeeper *global_config_nvs_keeper;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::config_nvs

#endif  // USE_ESP32
