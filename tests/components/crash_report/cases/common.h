#pragma once
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "esphome/components/crash_report/crash_report.h"
#include "esphome/components/logger/log_buffer.h"
#include "esphome/components/logger/logger.h"
#include "esphome/core/log.h"

namespace esphome::crash_report::testing {

static const char *const CRASH_TAG = "esp32.crash";

// A mounted directory, the way littlefs_storage presents the partition.
class FakeStorage : public filesystem_storage_abstract::FilesystemStorageAbstract {
 public:
  std::string path;
  bool mounted{true};
  bool is_mounted() const override { return this->mounted; }
  const std::string &get_base_path() const override { return this->path; }
  const char *get_filesystem_type() const override { return "Directory"; }
  bool request_format() override { return false; }
};

// Everything the process logs. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::pair<uint8_t, std::string>> lines;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() { this->lines.clear(); }
  bool has(uint8_t level, const char *needle) const {
    return std::any_of(this->lines.begin(), this->lines.end(), [level, needle](const auto &line) {
      return line.first == level && line.second.find(needle) != std::string::npos;
    });
  }

 protected:
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    static_cast<LogCapture *>(self)->lines.emplace_back(level, std::string(message, len));
  }
};

// The report over a stand-in for ESPHome's crash handler: the record is a list of lines it
// logs through the real logger, as crash_handler_log() does.
class TestCrashReport : public CrashReport {
 public:
  bool record{false};
  std::vector<std::pair<const char *, std::string>> lines;  // tag, text
  int clears{0};

  std::string header() const { return this->header_(); }
  void start_capture() {
    this->buffer_.clear();
    this->capturing_ = true;
  }
  std::string captured() const { return this->buffer_; }
  bool capturing() const { return this->capturing_; }

 protected:
  bool has_record_() override { return this->record; }
  void emit_record_() override {
    for (const auto &[tag, text] : this->lines)
      esp_log_printf_(ESPHOME_LOG_LEVEL_ERROR, tag, __LINE__, "%s", text.c_str());
  }
  void clear_record_() override { this->clears++; }
};

// One report for the whole run: the listener it registers points at it for good.
inline TestCrashReport &shared_report() {
  static auto *report = new TestCrashReport();
  return *report;
}

// A line as the logger hands it to its listeners: colour, "[E][tag:line]", an optional
// thread name, the text, the colour reset.
inline std::string formatted(uint8_t level, const char *tag, int line, const char *thread, const std::string &text) {
  char data[512];
  logger::LogBuffer buf{data, sizeof(data)};
  buf.write_header(level, tag, line, thread);
  buf.write_body(text.data(), text.size());
  return std::string(buf.data, buf.pos);
}

inline std::string read_file(const std::string &path) {
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace esphome::crash_report::testing
