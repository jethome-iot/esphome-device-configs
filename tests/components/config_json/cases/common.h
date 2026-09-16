#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>
#include "esphome/components/config_json/config_json.h"
#include "esphome/components/config_json/settings_base_json.h"
#include "esphome/components/logger/logger.h"

namespace esphome::config_json::testing {

// A mounted directory, the way littlefs_storage presents the partition.
class FakeStorage : public filesystem_storage_abstract::FilesystemStorageAbstract {
 public:
  std::string path;
  bool mounted{true};
  bool is_mounted() const override { return this->mounted; }
  const std::string &get_base_path() const override { return this->path; }
  const char *get_filesystem_type() const override { return "Directory"; }
};

// Every error and warning the process logs. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() {
    this->errors.clear();
    this->warnings.clear();
  }
  bool has(const std::vector<std::string> &lines, const char *needle) const {
    return std::any_of(lines.begin(), lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    auto *capture = static_cast<LogCapture *>(self);
    if (level == ESPHOME_LOG_LEVEL_ERROR) {
      capture->errors.emplace_back(message, len);
    } else if (level == ESPHOME_LOG_LEVEL_WARN) {
      capture->warnings.emplace_back(message, len);
    }
  }
};

// The smallest settings type the keeper can hold: a name, a flag and a number per record.
struct TestRecord {
  std::string source_name_;
  bool inverted{false};
  int level{0};

  const char *source_name() const { return this->source_name_.c_str(); }

  void to_json(JsonObject obj, uint32_t) const {
    obj["source_name"] = this->source_name_;
    obj["inverted"] = this->inverted;
    obj["level"] = this->level;
  }
  bool from_json(JsonObject obj, uint32_t) {
    if (!obj["source_name"].is<const char *>())
      return false;
    this->source_name_ = obj["source_name"].as<const char *>();
    this->inverted = obj["inverted"] | false;
    this->level = obj["level"] | 0;
    return true;
  }
};

class TestSettings : public SettingsBaseJsonTyped<TestSettings, TestRecord> {
  friend class SettingsBaseJsonTyped<TestSettings, TestRecord>;

 public:
  static constexpr const char *TAG = "test_settings";
  static constexpr float APPLY_PRIORITY = setup_priority::HARDWARE + 1.0f;
  const char *get_key() override { return "test"; }

  // What apply() handed over, in order.
  std::vector<std::string> applied;

  TestRecord *update_record(JsonObject) { return nullptr; }

  TestRecord *update(const std::string &name, bool inverted, int level) {
    TestRecord *record = nullptr;
    for (auto *r : this->records_) {
      if (r->source_name_ == name)
        record = r;
    }
    if (record == nullptr) {
      record = new TestRecord();  // NOLINT(cppcoreguidelines-owning-memory)
      record->source_name_ = name;
      this->records_.push_back(record);
    }
    record->inverted = inverted;
    record->level = level;
    this->mark_dirty();
    return record;
  }

  // "name=inverted/level ..." for a compact comparison.
  std::string report() const {
    std::string out;
    for (const auto *r : this->records_) {
      if (!out.empty())
        out += " ";
      out += r->source_name_ + "=" + (r->inverted ? "1" : "0") + "/" + std::to_string(r->level);
    }
    return out;
  }

 protected:
  void apply_record_(TestRecord *record) { this->applied.push_back(record->source_name_); }
};

}  // namespace esphome::config_json::testing
