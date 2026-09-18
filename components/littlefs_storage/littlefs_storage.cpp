#include "littlefs_storage.h"
#include "esphome/core/log.h"

#include <esp_littlefs.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace esphome::littlefs_storage {

static const char *const TAG = "littlefs_storage";

// Own namespace, not ESPHome's "esphome": the record has to be written after the preferences
// erase and must not look like a preference to anything else.
static const char *const NVS_NAMESPACE = "littlefs";
static const char *const NVS_KEY_WIPE = "wipe";

void LittleFSStorage::setup() {
  // Before the mount, so the wipe cannot pull the filesystem out from under an open file:
  // no component has set up yet and the web server does not exist.
  if (this->format_requested_()) {
    ESP_LOGI(TAG, "Wiping '%s' as the factory reset asked", this->partition_label_.c_str());
    esp_err_t err = esp_littlefs_format(this->partition_label_.c_str());
    if (err == ESP_OK) {
      this->clear_format_request_();
    } else {
      ESP_LOGE(TAG, "Wipe failed, retrying on the next boot: %s", esp_err_to_name(err));
    }
  }

  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = this->base_path_.c_str();
  conf.partition_label = this->partition_label_.c_str();
  conf.format_if_mount_failed = this->format_if_mount_failed_;
  conf.dont_mount = false;

  esp_err_t ret = esp_vfs_littlefs_register(&conf);
  if (ret != ESP_OK) {
    if (ret == ESP_ERR_NOT_FOUND) {
      ESP_LOGE(TAG, "Partition '%s' not found", this->partition_label_.c_str());
    } else {
      ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
    }
    this->mark_failed();
    return;
  }
  this->mounted_ = true;
}

void LittleFSStorage::dump_config() {
  ESP_LOGCONFIG(TAG, "LittleFS Storage:");
  ESP_LOGCONFIG(TAG, "  Partition: %s", this->partition_label_.c_str());
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->base_path_.c_str());
  ESP_LOGCONFIG(TAG, "  Format on mount fail: %s", YESNO(this->format_if_mount_failed_));
  ESP_LOGCONFIG(TAG, "  Mounted: %s", YESNO(this->mounted_));
  auto info = this->get_storage_info();
  if (info.valid) {
    ESP_LOGCONFIG(TAG, "  Used: %u / %u bytes", static_cast<unsigned>(info.used_bytes),
                  static_cast<unsigned>(info.total_bytes));
  }
}

bool LittleFSStorage::request_format() {
  // global_preferences->reset() deinitialises NVS on its way out; init is a no-op when it is
  // already up, which is the case on the way in from setup().
  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS unavailable, cannot record the wipe: %s", esp_err_to_name(err));
    return false;
  }
  nvs_handle_t handle;
  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Cannot record the wipe: %s", esp_err_to_name(err));
    return false;
  }
  err = nvs_set_u8(handle, NVS_KEY_WIPE, 1);
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Cannot record the wipe: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool LittleFSStorage::format_requested_() {
  if (nvs_flash_init() != ESP_OK)
    return false;
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    return false;
  uint8_t pending = 0;
  esp_err_t err = nvs_get_u8(handle, NVS_KEY_WIPE, &pending);
  nvs_close(handle);
  return err == ESP_OK && pending != 0;
}

void LittleFSStorage::clear_format_request_() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return;
  esp_err_t err = nvs_erase_key(handle, NVS_KEY_WIPE);
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    // Left standing, the record would wipe the partition again at every boot.
    ESP_LOGE(TAG, "Cannot clear the wipe record: %s", esp_err_to_name(err));
  }
}

filesystem_storage_abstract::StorageInfo LittleFSStorage::get_storage_info() const {
  filesystem_storage_abstract::StorageInfo info;
  if (!this->mounted_)
    return info;
  size_t total = 0, used = 0;
  if (esp_littlefs_info(this->partition_label_.c_str(), &total, &used) == ESP_OK) {
    info.total_bytes = total;
    info.used_bytes = used;
    info.free_bytes = total - used;
    info.valid = true;
  }
  return info;
}

}  // namespace esphome::littlefs_storage
