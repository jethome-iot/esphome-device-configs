#include "littlefs_storage.h"
#include "esphome/core/log.h"

#include <esp_littlefs.h>

namespace esphome::littlefs_storage {

static const char *const TAG = "littlefs_storage";

void LittleFSStorage::setup() {
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

bool LittleFSStorage::format() {
  // The library unmounts and frees every open file and directory struct on the way, so
  // everything using the filesystem is stopped first and given a moment to leave it.
  this->disable_access();
  esp_err_t err = esp_littlefs_format(this->partition_label_.c_str());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
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
