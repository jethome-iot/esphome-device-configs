#pragma once

#include <string>
#include <sys/stat.h>
#include "esphome/components/config_json/config_json.h"
#include "esphome/components/config_json/settings_base_json.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::test_settings {

// The same record shape jxd_config uses: keyed by object_id, applied to a switch. jxd_config
// itself is only_on_esp32, so the contract is exercised through this stand-in instead.
struct TestRecord {
  std::string source_name_;
  bool inverted{false};
  int level{0};
  uint32_t seen_version{0};

  const char *source_name() const { return this->source_name_.c_str(); }
  uint32_t key() const { return fnv1_hash(this->source_name_); }

  void to_json(JsonObject obj, uint32_t version) const {
    obj["source_name"] = this->source_name_;
    obj["inverted"] = this->inverted;
    obj["level"] = this->level;
  }

  bool from_json(JsonObject obj, uint32_t version) {
    if (!obj["source_name"].is<const char *>())
      return false;
    this->source_name_ = obj["source_name"].as<const char *>();
    this->inverted = obj["inverted"] | false;
    this->level = obj["level"] | 0;
    this->seen_version = version;
    return true;
  }
};

class TestSettingsJson : public config_json::SettingsBaseJsonTyped<TestSettingsJson, TestRecord> {
  friend class config_json::SettingsBaseJsonTyped<TestSettingsJson, TestRecord>;

 public:
  static constexpr const char *TAG = "test_settings";
  static constexpr const char *NAME = "test";
  static constexpr float APPLY_PRIORITY = setup_priority::HARDWARE + 1.0f;

  const char *get_key() override { return NAME; }

  TestRecord *update(const char *source_name, bool inverted, int level) {
    TestRecord *record = this->find_(source_name);
    if (record == nullptr) {
      record = new TestRecord();  // NOLINT(cppcoreguidelines-owning-memory)
      record->source_name_ = source_name;
      this->records_.push_back(record);
    }
    record->inverted = inverted;
    record->level = level;
    this->mark_dirty();
    return record;
  }

  // The REST-shaped edit: {"source_name": ..., "settings": {"inverted": ..., "level": ...}}.
  // Unknown entity -> nullptr, the way jxd_config rejects a POST.
  TestRecord *update_record(JsonObject obj) {
    const char *source_name = obj["source_name"];
    if (source_name == nullptr || this->find_switch_(fnv1_hash(source_name)) == nullptr)
      return nullptr;
    JsonObject settings = obj["settings"];
    return this->update(source_name, settings["inverted"] | false, settings["level"] | 0);
  }

  // One line the driver parses: the whole loaded state.
  std::string report() {
    std::string out = str_sprintf("n=%u", static_cast<unsigned>(this->records_.size()));
    for (const auto *record : this->records_) {
      out += str_sprintf(" %s=%d/%d/v%u", record->source_name(), record->inverted ? 1 : 0, record->level,
                         static_cast<unsigned>(record->seen_version));
    }
    return out;
  }

 protected:
  TestRecord *find_(const char *source_name) {
    for (auto *record : this->records_) {
      if (record->source_name_ == source_name)
        return record;
    }
    return nullptr;
  }

  // The lookup jxd_config makes: fnv1_hash(object_id) against the entity's own object_id hash.
  static switch_::Switch *find_switch_(uint32_t key) {
    for (auto *sw : App.get_switches()) {
      if (sw->get_object_id_hash() == key && !sw->is_internal())
        return sw;
    }
    return nullptr;
  }

  // A record naming an entity this build lacks is logged and skipped.
  void apply_record_(TestRecord *record) {
    if (record == nullptr)
      return;
    ESP_LOGI(TAG, "APPLY %s found=%d", record->source_name(), find_switch_(record->key()) != nullptr ? 1 : 0);
  }
};

// The driver checks a corrupt file is neither repaired nor clobbered; a rewrite changes the size.
inline long settings_file_size(filesystem_storage_abstract::FilesystemStorageAbstract *storage, const std::string &dir,
                               const char *key) {
  const std::string path = storage->get_base_path() + "/" + dir + "/" + key + ".json";
  struct stat st {};
  return stat(path.c_str(), &st) == 0 ? static_cast<long>(st.st_size) : -1;
}

}  // namespace esphome::test_settings
