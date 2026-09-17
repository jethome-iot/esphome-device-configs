#pragma once

#include <string>
#include "esphome/core/component.h"

namespace esphome::filesystem_storage_abstract {

struct StorageInfo {
  size_t total_bytes{0};
  size_t used_bytes{0};
  size_t free_bytes{0};
  bool valid{false};
};

// A mounted POSIX filesystem: everything below get_base_path() is reachable with stdio.
class FilesystemStorageAbstract : public Component {
 public:
  virtual bool is_mounted() const = 0;
  virtual const std::string &get_base_path() const = 0;
  virtual const char *get_filesystem_type() const { return "Unknown"; }
  virtual StorageInfo get_storage_info() const { return StorageInfo{}; }
};

}  // namespace esphome::filesystem_storage_abstract
