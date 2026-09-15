#pragma once

#include "esphome/core/defines.h"
#ifdef USE_ESP32

#include <nvs.h>
#include <nvs_flash.h>
#include <string>
#include "esphome/components/config_base/settings_base_common.h"

namespace esphome::config_nvs {

// One NVS namespace (name at most 15 chars).
class SettingsBaseNvs : public config_base::SettingsBaseCommon {
 public:
  ~SettingsBaseNvs() override = default;

  virtual const char *get_namespace() = 0;
  // True when loaded or when the namespace does not exist yet.
  virtual bool load_from_nvs() = 0;
  virtual bool save_to_nvs() = 0;

 protected:
  bool nvs_read_string_(nvs_handle_t handle, const char *key, std::string &value);
  bool nvs_write_string_(nvs_handle_t handle, const char *key, const std::string &value);
};

}  // namespace esphome::config_nvs

#endif  // USE_ESP32
