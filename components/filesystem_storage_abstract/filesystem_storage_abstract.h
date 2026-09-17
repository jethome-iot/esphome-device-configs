#pragma once

#include <atomic>
#include <string>
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

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

  // Use of the filesystem is latched off before the mount is wiped, and never back on: a
  // format frees every open file and directory struct, so a reader that carried on would
  // read through freed memory just as a writer would write through it.
  bool access_disabled() const { return this->access_disabled_; }

  // Refuses new calls, then waits out the ones already inside the filesystem. A caller that
  // yields re-takes its Access afterwards, so only a single libc call has to be waited for.
  void disable_access(uint32_t timeout_ms = 3000) {
    this->access_disabled_ = true;
    const uint32_t start = millis();
    while (this->in_flight_ > 0 && millis() - start < timeout_ms) {
      delay(5);
    }
    if (this->in_flight_ > 0)
      ESP_LOGW("storage", "%d filesystem call(s) still running after %ums", this->in_flight_.load(),
               static_cast<unsigned>(timeout_ms));
  }

  // Claims the filesystem for one call; falsy once access is latched off. Take it for the
  // call itself — one held across a yield would stall a format instead of letting it
  // through, and a handle kept across that yield is freed by the format anyway, so the
  // caller has to let go of it rather than close it.
  class Access {
   public:
    explicit Access(FilesystemStorageAbstract *storage) : storage_(storage) {
      if (this->storage_ == nullptr)
        return;
      // Claim first, then look: a claim that lands before the latch is one disable_access()
      // waits for, and one that lands after sees it and backs out.
      this->storage_->in_flight_++;
      if (this->storage_->access_disabled_) {
        this->storage_->in_flight_--;
        this->storage_ = nullptr;
      }
    }
    ~Access() {
      if (this->storage_ != nullptr)
        this->storage_->in_flight_--;
    }
    Access(const Access &) = delete;
    Access &operator=(const Access &) = delete;
    explicit operator bool() const { return this->storage_ != nullptr; }

   private:
    FilesystemStorageAbstract *storage_;
  };

 protected:
  std::atomic<bool> access_disabled_{false};
  std::atomic<int> in_flight_{0};
};

}  // namespace esphome::filesystem_storage_abstract
