#pragma once

#include <cstring>
#include <string>
#include <vector>
#include "esphome/components/config_base/settings_base_common.h"
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "esphome/components/json/json_util.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome::config_json {

static const uint32_t SETTINGS_FILE_VERSION = 1;

// One <key>.json file.
class SettingsBaseJson : public config_base::SettingsBaseCommon {
 public:
  ~SettingsBaseJson() override = default;

  virtual const char *get_key() = 0;

  virtual bool parse_json(JsonObject root, uint32_t version) = 0;
  virtual void write_json(JsonObject root, uint32_t version) = 0;

  // The record hooks: the per-record read/write/delete surface and the form description a device
  // dashboard drives. Nothing in this repository calls them yet — the dashboard is a later PR, and
  // the overrides stay so it does not have to reinstate them. The display menu edits records
  // through the typed lambda API instead.
  virtual void *update_record_from_json(JsonObject obj) { return nullptr; }
  virtual void apply_record(void *record) {}
  virtual void *can_delete(JsonObject obj) { return nullptr; }
  virtual bool delete_record(void *record) { return false; }
  virtual void write_json_single(JsonObject root, uint32_t version, const char *source_name) {}
  virtual void write_settings_meta(JsonObject obj) {}

  bool load_from_file(filesystem_storage_abstract::FilesystemStorageAbstract *storage, const std::string &dir_path);
  bool save_to_file(filesystem_storage_abstract::FilesystemStorageAbstract *storage, const std::string &dir_path);
};

// A list of records under "records". TRecord: from_json(JsonObject, uint32_t), to_json(JsonObject,
// uint32_t) const, source_name(). Derived: TAG, update_record(JsonObject), apply_record_(TRecord *).
template<typename Derived, typename TRecord> class SettingsBaseJsonTyped : public SettingsBaseJson {
 public:
  ~SettingsBaseJsonTyped() override { this->clear_records_(); }

  void *update_record_from_json(JsonObject obj) final { return static_cast<Derived *>(this)->update_record(obj); }

  void apply_record(void *record) final { static_cast<Derived *>(this)->apply_record_(static_cast<TRecord *>(record)); }

  bool parse_json(JsonObject root, uint32_t version) override {
    this->clear_records_();
    if (!root["records"].is<JsonArray>()) {
      ESP_LOGE(Derived::TAG, "Invalid 'records' field - must be an array");
      return false;
    }
    JsonArray array = root["records"];
    for (JsonObject obj : array) {
      TRecord *record = new TRecord();  // NOLINT(cppcoreguidelines-owning-memory)
      if (!record->from_json(obj, version)) {
        ESP_LOGW(Derived::TAG, "Failed to parse record");
        delete record;  // NOLINT(cppcoreguidelines-owning-memory)
        continue;
      }
      // One record per entity: a later duplicate replaces the earlier one in place, so what
      // apply() ends on and what an edit finds are the same record.
      TRecord **slot = this->find_slot_(record->source_name());
      if (slot != nullptr) {
        ESP_LOGW(Derived::TAG, "Duplicate record for '%s', keeping the last one", record->source_name());
        delete *slot;  // NOLINT(cppcoreguidelines-owning-memory)
        *slot = record;
      } else {
        this->records_.push_back(record);
      }
    }
    ESP_LOGI(Derived::TAG, "Loaded %u records", static_cast<unsigned>(this->records_.size()));
    return true;
  }

  void write_json(JsonObject root, uint32_t version) override {
    JsonArray array = root["records"].to<JsonArray>();
    for (const auto *record : this->records_) {
      if (record != nullptr)
        record->to_json(array.add<JsonObject>(), version);
    }
  }

  void write_json_single(JsonObject root, uint32_t version, const char *source_name) override {
    JsonArray array = root["records"].to<JsonArray>();
    for (const auto *record : this->records_) {
      if (record != nullptr && source_name_eq(record->source_name(), source_name)) {
        record->to_json(array.add<JsonObject>(), version);
        return;
      }
    }
  }

  void apply() override {
    for (auto *record : this->records_)
      static_cast<Derived *>(this)->apply_record_(record);
  }

  size_t size() override { return this->records_.size(); }

  void reset() override {
    this->clear_records_();
    this->mark_dirty();
    ESP_LOGI(Derived::TAG, "Cleared all records");
  }

  std::vector<TRecord *> &records() { return this->records_; }

 protected:
  void clear_records_() {
    for (auto *record : this->records_)
      delete record;  // NOLINT(cppcoreguidelines-owning-memory)
    this->records_.clear();
  }

  static bool source_name_eq(const char *a, const char *b) { return std::strcmp(a, b) == 0; }
  static bool source_name_eq(const std::string &a, const char *b) { return a == b; }

  TRecord **find_slot_(const char *source_name) {
    for (auto &record : this->records_) {
      if (record != nullptr && source_name_eq(record->source_name(), source_name))
        return &record;
    }
    return nullptr;
  }

  std::vector<TRecord *> records_;
};

}  // namespace esphome::config_json
