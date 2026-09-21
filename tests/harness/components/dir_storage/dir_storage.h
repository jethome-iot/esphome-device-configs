#pragma once

#include <string>
#include <sys/stat.h>
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "esphome/core/log.h"

namespace esphome::dir_storage {

class DirStorage : public filesystem_storage_abstract::FilesystemStorageAbstract {
 public:
  void setup() override {
    struct stat st;
    if (stat(this->base_path_.c_str(), &st) != 0) {
      if (mkdir(this->base_path_.c_str(), 0755) != 0) {
        ESP_LOGE("dir_storage", "Cannot create %s", this->base_path_.c_str());
        this->mark_failed();
        return;
      }
    } else if (!S_ISDIR(st.st_mode)) {
      // Reporting a file as mounted would run every filesystem test against a path
      // that cannot hold one.
      ESP_LOGE("dir_storage", "%s is not a directory", this->base_path_.c_str());
      this->mark_failed();
      return;
    }
    this->mounted_ = true;
  }
  float get_setup_priority() const override { return setup_priority::HARDWARE + 10.0f; }

  void set_base_path(const std::string &path) { this->base_path_ = path; }
  bool is_mounted() const override { return this->mounted_; }
  const std::string &get_base_path() const override { return this->base_path_; }
  const char *get_filesystem_type() const override { return "Directory"; }

  // What a factory reset calls. Nothing is wiped: the tests are about the request reaching
  // the storage, not about POSIX.
  bool request_format() override {
    this->format_requests++;
    return this->format_result;
  }

  int format_requests{0};
  bool format_result{true};

 protected:
  std::string base_path_;
  bool mounted_{false};
};

}  // namespace esphome::dir_storage
