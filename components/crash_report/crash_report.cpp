#include "crash_report.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esphome/components/logger/logger.h"
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#ifdef USE_ESP32
#include <esp_app_desc.h>
#endif
#ifdef USE_ESP32_CRASH_HANDLER
#include "esphome/components/esp32/crash_handler.h"
#elif defined(USE_ESP32)
// ESPHome leaves its crash handler out whenever Arduino is linked, even as an ESP-IDF component.
#error "crash_report needs ESPHome's crash handler, which is left out when Arduino is linked"
#endif

namespace esphome::crash_report {

static const char *const TAG = "crash_report";
// The tag crash_handler_log() logs under; only those lines belong in the file.
static const char *const CRASH_TAG = "esp32.crash";
// Room to spare over a worst-case dual-core record: 16 frames a core and both addr2line hints.
static const size_t MAX_REPORT_BYTES = 4096;

float CrashReport::get_setup_priority() const {
  // Straight after the storage mount (HARDWARE + 10) and before config_json (HARDWARE + 5), so a
  // component that panics in its own setup() still leaves a report behind.
  return setup_priority::HARDWARE + 9.0f;
}

#ifdef USE_ESP32_CRASH_HANDLER
bool CrashReport::has_record_() { return esp32::crash_handler_has_data(); }
void CrashReport::emit_record_() { esp32::crash_handler_log(); }
// Zeroes only the magic: API log clients later in this boot still get the record.
void CrashReport::clear_record_() { esp32::crash_handler_clear(); }
#else
bool CrashReport::has_record_() { return false; }
void CrashReport::emit_record_() {}
void CrashReport::clear_record_() {}
#endif

void CrashReport::on_log(void *instance, uint8_t level, const char *tag, const char *message, size_t len) {
  (void) level;
  auto *self = static_cast<CrashReport *>(instance);
  if (!self->capturing_ || strcmp(tag, CRASH_TAG) != 0)
    return;
  // Colour codes go first: with a thread name, one sits between its "]" and the ": ".
  std::string line;
  line.reserve(len);
  for (size_t i = 0; i < len; i++) {
    if (message[i] == '\033' && i + 1 < len && message[i + 1] == '[') {
      i += 2;
      while (i < len && !isalpha(static_cast<unsigned char>(message[i])))
        i++;
      continue;  // the loop steps over the final letter
    }
    line.push_back(message[i]);
  }
  const size_t body = line.find("]: ");
  if (body != std::string::npos)
    line.erase(0, body + 3);
  if (self->buffer_.size() + line.size() + 1 > MAX_REPORT_BYTES) {
    // Stop at the first line that does not fit: the file is a prefix of the record, never one
    // with a hole in it.
    self->capturing_ = false;
    return;
  }
  self->buffer_.append(line);
  self->buffer_.push_back('\n');
}

std::string CrashReport::header_() const {
  std::string out = "firmware: ";
#ifdef ESPHOME_PROJECT_NAME
  out += ESPHOME_PROJECT_NAME " " ESPHOME_PROJECT_VERSION;
#else
  out += App.get_name().str() + " " ESPHOME_VERSION;
#endif
  char build_time[Application::BUILD_TIME_STR_SIZE];
  App.get_build_time_string(build_time);
  out += "\nbuild: ";
  out += build_time;
  out += '\n';
#ifdef USE_ESP32
  // Not esp_app_get_elf_sha256(): it stops at CONFIG_APP_RETRIEVE_LEN_ELF_SHA, 9 by default.
  const esp_app_desc_t *desc = esp_app_get_description();
  char sha[sizeof(desc->app_elf_sha256) * 2 + 1];
  format_hex_to(sha, desc->app_elf_sha256, sizeof(desc->app_elf_sha256));
  out += "elf_sha256: ";
  out += sha;
  out += '\n';
#endif
  return out;
}

std::string CrashReport::dir_path_() const { return this->storage_->get_base_path() + "/" + this->report_dir_; }

void CrashReport::setup() {
  if (!this->has_record_()) {
    ESP_LOGD(TAG, "No crash record from the previous boot");
    return;
  }
  if (this->storage_ == nullptr || !this->storage_->is_mounted()) {
    ESP_LOGW(TAG, "Crash record found but storage is not mounted; it stays in RAM only");
    return;
  }

  // Once: the logger keeps its listeners for good.
  if (!this->listening_) {
    logger::global_logger->add_log_callback(this, &CrashReport::on_log);
    this->listening_ = true;
  }
  this->buffer_ = this->header_();
  const size_t header_len = this->buffer_.size();
  this->capturing_ = true;
  this->emit_record_();  // re-emits the record; our callback collects it
  this->capturing_ = false;

  if (this->buffer_.size() == header_len) {
    ESP_LOGW(TAG, "Crash record reported no lines");
  } else if (this->write_report_()) {
    // Only once it is on disk: a failed write retries at the next boot.
    this->clear_record_();
  }
  std::string().swap(this->buffer_);  // needed once per boot
}

bool CrashReport::rotate_() {
  const std::string dir = this->dir_path_();
  // Oldest first: keep-1 is dropped, the rest shift up one, each onto the name the step before
  // freed. ENOENT is a slot not used yet; anything else stops the shift before a rename can
  // replace a report still kept.
  std::string oldest = dir + "/crash" + std::to_string(this->keep_ - 1) + ".txt";
  if (::remove(oldest.c_str()) != 0 && errno != ENOENT) {
    ESP_LOGE(TAG, "Cannot remove '%s'", oldest.c_str());
    return false;
  }
  for (int i = this->keep_ - 2; i >= 0; i--) {
    std::string from = dir + "/crash" + std::to_string(i) + ".txt";
    std::string to = dir + "/crash" + std::to_string(i + 1) + ".txt";
    if (::rename(from.c_str(), to.c_str()) != 0 && errno != ENOENT) {
      ESP_LOGE(TAG, "Cannot move '%s' to '%s'", from.c_str(), to.c_str());
      return false;
    }
  }
  return true;
}

bool CrashReport::write_report_() {
  const std::string dir = this->dir_path_();
  struct stat st {};
  if (::stat(dir.c_str(), &st) != 0 && ::mkdir(dir.c_str(), 0777) != 0) {
    ESP_LOGE(TAG, "Cannot create '%s'", dir.c_str());
    return false;
  }
  // Written aside first, so a write that fails leaves the reports already kept where they were.
  const std::string tmp = dir + "/crash.tmp";
  FILE *f = ::fopen(tmp.c_str(), "wb");
  if (f == nullptr) {
    ESP_LOGE(TAG, "Cannot open '%s'", tmp.c_str());
    return false;
  }
  const size_t written = ::fwrite(this->buffer_.data(), 1, this->buffer_.size(), f);
  // LittleFS commits at close, so a full filesystem is reported here.
  const bool ok = ::fclose(f) == 0 && written == this->buffer_.size();
  if (!ok) {
    ESP_LOGE(TAG, "Failed to write '%s'", tmp.c_str());
    ::remove(tmp.c_str());
    return false;
  }
  if (!this->rotate_()) {
    ::remove(tmp.c_str());
    return false;
  }

  const std::string path = dir + "/crash0.txt";
  // Only a flash error fails this, after the shift: the others have moved up and the oldest is gone.
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ESP_LOGE(TAG, "Cannot rename '%s' to '%s'", tmp.c_str(), path.c_str());
    ::remove(tmp.c_str());
    return false;
  }
  ESP_LOGW(TAG, "Crash report saved to '%s' (%u bytes)", path.c_str(), static_cast<unsigned>(written));
  return true;
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
