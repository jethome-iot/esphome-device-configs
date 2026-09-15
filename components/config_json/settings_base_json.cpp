#include "settings_base_json.h"
#include <cstdio>

namespace esphome::config_json {

static const char *const TAG = "config_json.base";
static const long MAX_FILE_BYTES = 65536;

bool SettingsBaseJson::load_from_file(filesystem_storage_abstract::FilesystemStorageAbstract *storage,
                                      const std::string &dir_path) {
  if (storage == nullptr) {
    ESP_LOGE(TAG, "Storage backend is null");
    return false;
  }
  std::string full_path = storage->get_base_path() + "/" + dir_path + "/" + this->get_key() + ".json";

  FILE *file = fopen(full_path.c_str(), "r");
  if (file == nullptr) {
    ESP_LOGD(TAG, "File '%s' does not exist, starting with empty settings", full_path.c_str());
    return true;
  }

  fseek(file, 0, SEEK_END);
  const long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (file_size <= 0 || file_size > MAX_FILE_BYTES) {
    ESP_LOGW(TAG, "Invalid file size: %ld bytes in '%s'", file_size, full_path.c_str());
    fclose(file);
    return false;
  }

  std::string json_data;
  json_data.resize(file_size);
  const size_t read_size = fread(&json_data[0], 1, file_size, file);
  fclose(file);
  if (read_size != static_cast<size_t>(file_size)) {
    ESP_LOGE(TAG, "Failed to read file '%s' completely", full_path.c_str());
    return false;
  }

  JsonDocument doc = json::parse_json(reinterpret_cast<const uint8_t *>(json_data.c_str()), json_data.size());
  if (doc.overflowed() || doc.isNull() || !doc.is<JsonObject>()) {
    ESP_LOGE(TAG, "Failed to parse JSON from '%s'", full_path.c_str());
    return false;
  }

  JsonObject root = doc.as<JsonObject>();
  const uint32_t version = root["version"] | SETTINGS_FILE_VERSION;
  if (!this->parse_json(root, version)) {
    ESP_LOGE(TAG, "Failed to parse %s settings from '%s'", this->get_key(), full_path.c_str());
    return false;
  }
  ESP_LOGI(TAG, "Loaded %s settings from '%s' (version %u)", this->get_key(), full_path.c_str(),
           static_cast<unsigned>(version));
  return true;
}

bool SettingsBaseJson::save_to_file(filesystem_storage_abstract::FilesystemStorageAbstract *storage,
                                    const std::string &dir_path) {
  if (storage == nullptr) {
    ESP_LOGE(TAG, "Storage backend is null");
    return false;
  }
  std::string full_path = storage->get_base_path() + "/" + dir_path + "/" + this->get_key() + ".json";

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["version"] = SETTINGS_FILE_VERSION;
  this->write_json(root, SETTINGS_FILE_VERSION);
  std::string json_data;
  serializeJson(doc, json_data);

  FILE *file = fopen(full_path.c_str(), "w");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open '%s' for writing", full_path.c_str());
    return false;
  }
  const size_t written = fwrite(json_data.c_str(), 1, json_data.length(), file);
  fclose(file);
  if (written != json_data.length()) {
    ESP_LOGE(TAG, "Failed to write complete data to '%s'", full_path.c_str());
    return false;
  }

  ESP_LOGD(TAG, "Saved %s settings to '%s' (%u bytes)", this->get_key(), full_path.c_str(),
           static_cast<unsigned>(json_data.length()));
  this->clear_dirty();
  return true;
}

}  // namespace esphome::config_json
