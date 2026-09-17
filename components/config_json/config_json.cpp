#include "config_json.h"
#include <sys/stat.h>
#include "esphome/core/log.h"

namespace esphome::config_json {

ConfigJsonKeeper *global_config_json_keeper = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

ConfigJsonKeeper::ConfigJsonKeeper() { global_config_json_keeper = this; }

void ConfigJsonKeeper::setup() {
  if (this->storage_backend_ == nullptr) {
    ESP_LOGE(TAG, "Filesystem storage not configured");
    this->mark_failed();
    return;
  }
  if (!this->storage_backend_->is_mounted()) {
    ESP_LOGE(TAG, "%s not mounted", this->storage_backend_->get_filesystem_type());
    this->mark_failed();
    return;
  }
  if (!this->ensure_directory_exists_())
    ESP_LOGW(TAG, "Could not create config directory, will try on first save");
  this->setup_common_();
}

void ConfigJsonKeeper::dump_config() {
  ESP_LOGCONFIG(TAG, "Config JSON:");
  if (this->storage_backend_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Storage: %s", this->storage_backend_->get_filesystem_type());
    ESP_LOGCONFIG(TAG, "  Config dir: %s/%s/", this->storage_backend_->get_base_path().c_str(),
                  this->config_dir_.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Save delay: %u ms", static_cast<unsigned>(this->save_delay_ms_));
  for (auto *settings : this->settings_list_) {
    if (settings != nullptr)
      ESP_LOGCONFIG(TAG, "  %s.json: %u records", settings->get_key(), static_cast<unsigned>(settings->size()));
  }
}

void ConfigJsonKeeper::load_one(SettingsBaseJson *settings) {
  settings->load_from_file(this->storage_backend_, this->config_dir_);
}

void ConfigJsonKeeper::save_one(SettingsBaseJson *settings) {
  if (this->storage_backend_ == nullptr) {
    ESP_LOGE(TAG, "Cannot save: filesystem storage not configured");
    return;
  }
  this->ensure_directory_exists_();
  settings->save_to_file(this->storage_backend_, this->config_dir_);
}

bool ConfigJsonKeeper::ensure_directory_exists_() {
  if (this->storage_backend_ == nullptr)
    return false;
  filesystem_storage_abstract::FilesystemStorageAbstract::Access use(this->storage_backend_);
  if (!use)
    return false;
  std::string full_dir = this->storage_backend_->get_base_path() + "/" + this->config_dir_;
  struct stat st {};
  if (stat(full_dir.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode))
      return true;
    ESP_LOGE(TAG, "Path '%s' exists but is not a directory", full_dir.c_str());
    return false;
  }
  if (mkdir(full_dir.c_str(), 0755) == 0) {
    ESP_LOGI(TAG, "Created config directory: %s", full_dir.c_str());
    return true;
  }
  ESP_LOGE(TAG, "Failed to create directory '%s'", full_dir.c_str());
  return false;
}

}  // namespace esphome::config_json
