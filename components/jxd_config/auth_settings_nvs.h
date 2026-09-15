#pragma once

#include "esphome/core/defines.h"
#if defined(JXD_CONFIG_AUTH) && defined(USE_ESP32)

#include <string>
#include <vector>
#include "esphome/components/config_nvs/settings_base_nvs.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/log.h"

namespace esphome::jxd_config {

// Web server basic-auth credentials; override the compiled ones while stored.
class AuthSettingsNvs : public config_nvs::SettingsBaseNvs {
 public:
  static constexpr const char *TAG = "jxd_config.auth";
  static constexpr const char *NAMESPACE = "web_auth";

  const char *get_namespace() override { return NAMESPACE; }

  bool load_from_nvs() override {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGD(TAG, "No credentials stored");
      return true;
    }
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NAMESPACE, esp_err_to_name(err));
      return false;
    }
    this->nvs_read_string_(handle, "username", this->username_);
    this->nvs_read_string_(handle, "password", this->password_);
    nvs_close(handle);
    this->loaded_ = !this->username_.empty() && !this->password_.empty();
    if (this->loaded_)
      ESP_LOGI(TAG, "Loaded credentials for user '%s'", this->username_.c_str());
    return true;
  }

  bool save_to_nvs() override {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to open NVS namespace '%s' for writing: %s", NAMESPACE, esp_err_to_name(err));
      return false;
    }
    bool success = this->nvs_write_string_(handle, "username", this->username_) &&
                   this->nvs_write_string_(handle, "password", this->password_);
    if (success) {
      err = nvs_commit(handle);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        success = false;
      }
    }
    nvs_close(handle);
    if (success) {
      this->clear_dirty();
      ESP_LOGI(TAG, "Saved credentials for user '%s'", this->username_.c_str());
    }
    return success;
  }

  void apply() override {
#ifdef USE_WEBSERVER_AUTH
    if (!this->loaded_)
      return;
    auto *wsb = web_server_base::global_web_server_base;
    if (wsb == nullptr) {
      ESP_LOGW(TAG, "No web_server_base to apply credentials to");
      return;
    }
    // The server keeps bare pointers and checks them from its own task, so the strings it is
    // handed are never freed or resized; a change hands it fresh copies.
    auto *username = new std::string(this->username_);  // NOLINT(cppcoreguidelines-owning-memory)
    auto *password = new std::string(this->password_);  // NOLINT(cppcoreguidelines-owning-memory)
    wsb->set_auth_username(username->c_str());
    wsb->set_auth_password(password->c_str());
    this->applied_ = true;
    ESP_LOGD(TAG, "Applied credentials for user '%s'", this->username_.c_str());
#endif
  }

  static constexpr float APPLY_PRIORITY = setup_priority::HARDWARE + 1.0f;

  size_t size() override { return this->loaded_ ? 1 : 0; }

  // Back to the compiled credentials at the next boot; the running server keeps the current ones.
  void reset() override {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
      nvs_erase_all(handle);
      nvs_commit(handle);
      nvs_close(handle);
    }
    this->username_.clear();
    this->password_.clear();
    this->loaded_ = false;
    ESP_LOGI(TAG, "Cleared credentials");
  }

  // Takes effect at once when the server already runs on stored credentials, else at the next boot.
  void set_credentials(const std::string &username, const std::string &password) {
    if (this->loaded_ && this->username_ == username && this->password_ == password)
      return;
    this->username_ = username;
    this->password_ = password;
    this->loaded_ = true;
    this->mark_dirty();
    if (this->applied_)
      this->apply();
  }

  bool is_loaded() const { return this->loaded_; }
  const std::string &username() const { return this->username_; }
  size_t password_len() const { return this->password_.size(); }

 protected:
  std::string username_;
  std::string password_;
  bool loaded_{false};
  bool applied_{false};
};

}  // namespace esphome::jxd_config

#endif  // JXD_CONFIG_AUTH && USE_ESP32
