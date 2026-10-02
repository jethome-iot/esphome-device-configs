#pragma once

#include <string>
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "esphome/core/component.h"

namespace esphome::crash_report {

// Copies the panic record ESPHome's crash handler left in .noinit onto the storage mount, so a
// device that panicked in the field can hand the report back over HTTP instead of only to a
// console or an `esphome logs` client that happened to be attached.
class CrashReport : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_storage(filesystem_storage_abstract::FilesystemStorageAbstract *storage) { storage_ = storage; }
  void set_report_dir(const std::string &dir) { report_dir_ = dir; }
  void set_keep(uint8_t keep) { keep_ = keep; }

  // Log listener trampoline: only collects while capturing_ is set.
  static void on_log(void *instance, uint8_t level, const char *tag, const char *message, size_t len);

 protected:
  // The crash handler behind a seam: it exists only on ESP32, and the host tests stand in for it.
  virtual bool has_record_();
  virtual void emit_record_();
  virtual void clear_record_();

  std::string header_() const;
  bool write_report_();
  bool rotate_();
  std::string dir_path_() const;

  filesystem_storage_abstract::FilesystemStorageAbstract *storage_{nullptr};
  std::string report_dir_;
  std::string buffer_;
  uint8_t keep_{4};
  bool capturing_{false};
  bool listening_{false};
};

}  // namespace esphome::crash_report
