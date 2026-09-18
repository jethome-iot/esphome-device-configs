#pragma once

#include <string>
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "esphome/core/component.h"

namespace esphome::littlefs_storage {

class LittleFSStorage : public filesystem_storage_abstract::FilesystemStorageAbstract {
 public:
  void setup() override;
  void dump_config() override;
  // Before every component that stores files.
  float get_setup_priority() const override { return setup_priority::HARDWARE + 10.0f; }

  void set_partition_label(const std::string &label) { this->partition_label_ = label; }
  void set_base_path(const std::string &path) { this->base_path_ = path; }
  void set_format_if_mount_failed(bool format) { this->format_if_mount_failed_ = format; }

  // Records a wipe of the partition, carried out at the next boot before the mount, where no
  // file or directory can be open on it. Call it after global_preferences->reset(), which
  // erases the whole NVS partition and the record with it.
  bool request_format();

  bool is_mounted() const override { return this->mounted_; }
  const std::string &get_base_path() const override { return this->base_path_; }
  const char *get_filesystem_type() const override { return "LittleFS"; }
  filesystem_storage_abstract::StorageInfo get_storage_info() const override;

 protected:
  // The record outlives the reboot in NVS, so it is read before the mount and cleared only
  // once the format it asked for succeeded: an interrupted one is retried on the next boot.
  bool format_requested_();
  void clear_format_request_();

  std::string partition_label_;
  std::string base_path_;
  bool format_if_mount_failed_{true};
  bool mounted_{false};
};

}  // namespace esphome::littlefs_storage
