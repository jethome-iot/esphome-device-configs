#include "littlefs_storage.h"
#include "esphome/core/log.h"

#include <esp_littlefs.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace esphome::littlefs_storage {

static const char *const TAG = "littlefs_storage";

// Not ESPHome's "esphome" namespace: the record is written after the preferences erase.
static const char *const NVS_NAMESPACE = "littlefs";
static const char *const NVS_KEY_WIPE = "wipe";

void LittleFSStorage::setup() {
  // Before the mount: nothing has set up yet, so nothing can hold a file on what this erases.
  if (this->format_requested_()) {
    ESP_LOGI(TAG, "Wiping '%s' as the factory reset asked", this->partition_label_.c_str());
    esp_err_t err = esp_littlefs_format(this->partition_label_.c_str());
    if (err != ESP_OK) {
      // Mounting now would serve the files the reset promised to erase, on a device that came
      // up on its factory credentials. The request stays recorded, so the next boot retries.
      ESP_LOGE(TAG, "Wipe failed, leaving '%s' unmounted until a boot manages it: %s", this->partition_label_.c_str(),
               esp_err_to_name(err));
      this->mark_failed(LOG_STR("The factory reset could not wipe the storage"));
      return;
    }
    if (!this->clear_format_request_()) {
      // Mounting now would take the files the user writes during this boot and erase them at
      // the next, which still finds the request standing — and say nothing either time. The
      // partition is already empty, so a boot that manages to clear the record loses nothing.
      ESP_LOGE(TAG, "Wipe recorded as still pending, leaving '%s' unmounted until a boot clears it",
               this->partition_label_.c_str());
      this->mark_failed(LOG_STR("The factory reset could not clear its own record"));
      return;
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
      this->mark_failed(LOG_STR("Storage partition not found"));
    } else {
      ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
      this->mark_failed(LOG_STR("The storage partition could not be mounted"));
    }
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
  esp_err_t err = this->record_format_request_();
  if (err == ESP_OK)
    return true;

  // An unrecorded request is never retried, and the caller reboots in a moment into a device
  // whose credentials went with the preferences: wipe here instead, slow as it is. The library
  // formats the live mount unlocked and frees every open file struct without closing it, so a
  // file another task holds at this instant is a crash, not an EBADF. Tolerable only because
  // the wipe itself still happens and the caller reboots either way.
  ESP_LOGE(TAG, "Cannot record the wipe (%s); wiping '%s' now", esp_err_to_name(err), this->partition_label_.c_str());
  err = esp_littlefs_format(this->partition_label_.c_str());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Wipe failed, '%s' is left as it was: %s", this->partition_label_.c_str(), esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "Wiped '%s'", this->partition_label_.c_str());
  return true;
}

esp_err_t LittleFSStorage::record_format_request_() {
  // reset() deinitialises NVS on its way out; init is a no-op when it is already up.
  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK)
    return err;
  nvs_handle_t handle;
  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK)
    return err;
  err = nvs_set_u8(handle, NVS_KEY_WIPE, 1);
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  return err;
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

bool LittleFSStorage::clear_format_request_() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Cannot clear the wipe record: %s", esp_err_to_name(err));
    return false;
  }
  err = nvs_erase_key(handle, NVS_KEY_WIPE);
  // Gone is the state this asks for, however it got there.
  if (err == ESP_ERR_NVS_NOT_FOUND)
    err = ESP_OK;
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    // Left standing, it would wipe the partition at every boot.
    ESP_LOGE(TAG, "Cannot clear the wipe record: %s", esp_err_to_name(err));
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
