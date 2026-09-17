#pragma once

#include <atomic>
#include <string>
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

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

  // Writing is latched off before the mount is wiped, and never back on: a format frees
  // every open file struct, so a writer that carried on would write through freed memory.
  // Reads are left alone. Writers hold a Write for the call they are about to make.
  bool writes_disabled() const { return this->writes_disabled_; }

  // Refuses new writes, then waits out the ones already inside the filesystem.
  void disable_writes(uint32_t timeout_ms = 1000) {
    this->writes_disabled_ = true;
    const uint32_t start = millis();
    while (this->writes_in_flight_ > 0 && millis() - start < timeout_ms) {
      delay(5);
    }
  }

  // Claims the filesystem for one write; falsy once writes are latched off. Take it for
  // the call itself — one held across a yield would stall a format instead of letting it
  // through, and the writer has to re-take it after every yield anyway.
  class Write {
   public:
    explicit Write(FilesystemStorageAbstract *storage) : storage_(storage) {
      if (this->storage_ == nullptr)
        return;
      // Claim first, then look: a claim that lands before the latch is one disable_writes()
      // waits for, and one that lands after sees it and backs out.
      this->storage_->writes_in_flight_++;
      if (this->storage_->writes_disabled_) {
        this->storage_->writes_in_flight_--;
        this->storage_ = nullptr;
      }
    }
    ~Write() {
      if (this->storage_ != nullptr)
        this->storage_->writes_in_flight_--;
    }
    Write(const Write &) = delete;
    Write &operator=(const Write &) = delete;
    explicit operator bool() const { return this->storage_ != nullptr; }

   private:
    FilesystemStorageAbstract *storage_;
  };

 protected:
  std::atomic<bool> writes_disabled_{false};
  std::atomic<int> writes_in_flight_{0};
};

}  // namespace esphome::filesystem_storage_abstract
