#pragma once

#include <string>
#include "esphome/components/config_base/config_keeper_base.h"
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "esphome/core/component.h"
#include "settings_base_json.h"

namespace esphome::config_json {

static const char *const TAG = "config_json";

// One JSON file per settings type under <base_path>/<config_dir>/.
class ConfigJsonKeeper : public config_base::ConfigKeeperBase<ConfigJsonKeeper, SettingsBaseJson> {
 public:
  ConfigJsonKeeper();

  void setup() override;
  void dump_config() override;

  void set_config_dir(const std::string &dir) { this->config_dir_ = dir; }
  void set_storage(filesystem_storage_abstract::FilesystemStorageAbstract *storage) {
    this->storage_backend_ = storage;
  }
  const std::string &get_config_dir() const { return this->config_dir_; }

  const char *get_timeout_name() const { return "config_save"; }
  const char *get_log_tag() const { return TAG; }
  const char *get_settings_key(SettingsBaseJson *settings) const { return settings->get_key(); }
  void load_one(SettingsBaseJson *settings);
  void save_one(SettingsBaseJson *settings);

 protected:
  bool ensure_directory_exists_();

  std::string config_dir_{"config"};
  filesystem_storage_abstract::FilesystemStorageAbstract *storage_backend_{nullptr};
};

extern ConfigJsonKeeper *global_config_json_keeper;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::config_json
