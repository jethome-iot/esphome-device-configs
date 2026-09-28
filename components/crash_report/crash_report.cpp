#include "crash_report.h"

#include <cstdio>
#include <sys/stat.h>

#include "esphome/components/esp32/crash_handler.h"
#include "esphome/components/logger/logger.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::crash_report {

static const char *const TAG = "crash_report";
// The tag crash_handler_log() logs under; only those lines belong in the file.
static const char *const CRASH_TAG = "esp32.crash";
static const size_t MAX_REPORT_BYTES = 2048;

float CrashReport::get_setup_priority() const {
  // After the storage mount (HARDWARE + 10) and before anything that serves files.
  return setup_priority::HARDWARE + 4.0f;
}

void CrashReport::on_log(void *instance, uint8_t level, const char *tag, const char *message, size_t len) {
  (void) level;
  auto *self = static_cast<CrashReport *>(instance);
  if (!self->capturing_ || strcmp(tag, CRASH_TAG) != 0)
    return;
  if (self->buffer_.size() + len + 1 > MAX_REPORT_BYTES)
    return;
  self->buffer_.append(message, len);
  self->buffer_.push_back('\n');
}

std::string CrashReport::dir_path_() const { return this->storage_->get_base_path() + "/" + this->report_dir_; }

void CrashReport::setup() {
  if (!esp32::crash_handler_has_data()) {
    ESP_LOGD(TAG, "No crash record from the previous boot");
    return;
  }
  if (this->storage_ == nullptr || !this->storage_->is_mounted()) {
    ESP_LOGW(TAG, "Crash record found but storage is not mounted; it stays in RAM only");
    return;
  }

  logger::global_logger->add_log_callback(this, &CrashReport::on_log);
  this->buffer_.clear();
  this->capturing_ = true;
  esp32::crash_handler_log();  // re-emits the record; our callback collects it
  this->capturing_ = false;

  if (this->buffer_.empty()) {
    ESP_LOGW(TAG, "Crash record reported no lines");
    return;
  }
  this->write_report_();
}

void CrashReport::rotate_() {
  const std::string dir = this->dir_path_();
  // Oldest first: keep-1 is dropped, the rest shift up one.
  std::string oldest = dir + "/crash" + std::to_string(this->keep_ - 1) + ".txt";
  ::remove(oldest.c_str());
  for (int i = this->keep_ - 2; i >= 0; i--) {
    std::string from = dir + "/crash" + std::to_string(i) + ".txt";
    std::string to = dir + "/crash" + std::to_string(i + 1) + ".txt";
    ::rename(from.c_str(), to.c_str());
  }
}

void CrashReport::write_report_() {
  const std::string dir = this->dir_path_();
  struct stat st {};
  if (::stat(dir.c_str(), &st) != 0 && ::mkdir(dir.c_str(), 0777) != 0) {
    ESP_LOGE(TAG, "Cannot create '%s'", dir.c_str());
    return;
  }
  this->rotate_();

  const std::string path = dir + "/crash0.txt";
  FILE *f = ::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    ESP_LOGE(TAG, "Cannot open '%s'", path.c_str());
    return;
  }
  const size_t written = ::fwrite(this->buffer_.data(), 1, this->buffer_.size(), f);
  // LittleFS commits at close, so a full filesystem is reported here.
  const bool ok = ::fclose(f) == 0 && written == this->buffer_.size();
  if (!ok) {
    ESP_LOGE(TAG, "Failed to write '%s'", path.c_str());
    ::remove(path.c_str());
    return;
  }
  ESP_LOGW(TAG, "Crash report saved to '%s' (%u bytes)", path.c_str(), static_cast<unsigned>(written));
}

void CrashReport::dump_config() {
  ESP_LOGCONFIG(TAG, "Crash report:");
  if (this->storage_ == nullptr || !this->storage_->is_mounted()) {
    ESP_LOGCONFIG(TAG, "  Storage not mounted");
    return;
  }
  ESP_LOGCONFIG(TAG, "  Directory: %s", this->dir_path_().c_str());
  ESP_LOGCONFIG(TAG, "  Keep: %u", this->keep_);
}

}  // namespace esphome::crash_report
