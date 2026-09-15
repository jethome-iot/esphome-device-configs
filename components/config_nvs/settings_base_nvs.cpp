#include "settings_base_nvs.h"
#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::config_nvs {

static const char *const TAG = "config_nvs.base";

bool SettingsBaseNvs::nvs_read_string_(nvs_handle_t handle, const char *key, std::string &value) {
  size_t len = 0;
  esp_err_t err = nvs_get_str(handle, key, nullptr, &len);
  if (err == ESP_ERR_NVS_NOT_FOUND)
    return true;
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to get size for key '%s': %s", key, esp_err_to_name(err));
    return false;
  }
  if (len == 0) {
    value.clear();
    return true;
  }
  value.resize(len - 1);  // len counts the terminator
  err = nvs_get_str(handle, key, &value[0], &len);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to read key '%s': %s", key, esp_err_to_name(err));
    value.clear();
    return false;
  }
  return true;
}

bool SettingsBaseNvs::nvs_write_string_(nvs_handle_t handle, const char *key, const std::string &value) {
  esp_err_t err = nvs_set_str(handle, key, value.c_str());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write key '%s': %s", key, esp_err_to_name(err));
    return false;
  }
  return true;
}

}  // namespace esphome::config_nvs

#endif  // USE_ESP32
