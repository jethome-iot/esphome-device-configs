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

  // Asks for everything below get_base_path() to be wiped; the backend decides whether that
  // happens here or at the next boot, so the caller must reboot straight after either way.
  // False is the backend's last word: nothing was wiped and nothing will try again. No
  // default: a factory reset reports beforehand that it clears the storage, so every backend
  // has to answer for that rather than silently doing nothing.
  virtual bool request_format() = 0;
};

}  // namespace esphome::filesystem_storage_abstract
