#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/config_json/config_json.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/network_config/ip4.h"
#include "esphome/components/network_config/network_config.h"
#include "esphome/components/network_config/network_settings_json.h"
#include "esphome/components/select/select.h"
#include "esphome/core/preferences.h"

namespace esphome::network_config::testing {

// The preferences manager the generated setup would have installed, emptied for each test: the
// component reads ESPHome's WiFi credential slot through it.
inline void reset_preferences() {
  host::setup_preferences();
  global_preferences->sync();
  global_preferences->reset();
}

// The component with the inbox drain reachable: what the poll calls every INBOX_POLL_MS.
class TestConfig : public NetworkConfig {
 public:
  using NetworkConfig::NetworkConfig;
  using NetworkConfig::drain_inbox_;
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
  bool has_warning(const char *needle) const { return this->has(this->warnings, needle); }
  bool has_error(const char *needle) const { return this->has(this->errors, needle); }

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

inline LogCapture &log() { return LogCapture::instance(); }

// A mounted directory, the way littlefs_storage presents the partition.
class FakeStorage : public filesystem_storage_abstract::FilesystemStorageAbstract {
 public:
  std::string path;
  bool is_mounted() const override { return true; }
  const std::string &get_base_path() const override { return this->path; }
  const char *get_filesystem_type() const override { return "Directory"; }
  // Never called here: a factory reset is the dashboard's and the menu's business.
  bool request_format() override { return false; }
};

// The "Network mode" select as network.yaml declares it.
class TestSelect : public select::Select {
 public:
  TestSelect() { this->traits.set_options({"Auto", "Ethernet", "WiFi"}); }
};

// A record as a firmware with both interfaces builds it. The host build has neither, so the
// availability the REST surface goes by is set here.
class Record : public ::testing::Test {
 protected:
  void SetUp() override {
    log().clear();
    this->settings.set_available_interfaces(true, true);
  }

  // What load_from_file() does with a document it managed to read.
  bool load(const std::string &text) {
    JsonDocument doc;
    if (deserializeJson(doc, text) != DeserializationError::Ok)
      return false;
    JsonObject root = doc.as<JsonObject>();
    return this->settings.parse_json(root, root["version"] | 1U);
  }

  // The refusal, or nullptr. The messages are literals, so the document may go out of scope.
  const char *update(const std::string &text) {
    JsonDocument doc;
    EXPECT_EQ(deserializeJson(doc, text), DeserializationError::Ok) << text;
    return this->settings.update_from_json(doc.as<JsonObject>());
  }

  std::string file() {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["version"] = 1;
    this->settings.write_json(root, 1);
    std::string out;
    serializeJson(doc, out);
    return out;
  }

  std::string public_json() {
    JsonDocument doc;
    this->settings.write_public_json(doc.to<JsonObject>());
    std::string out;
    serializeJson(doc, out);
    return out;
  }

  // "mode ssid:password ..." - a compact comparison of what a refusal must not change.
  std::string report() {
    std::string out = network_type_to_string(this->settings.network_type());
    for (const auto &network : this->settings.networks())
      out += " " + network.ssid + ":" + network.password;
    out += " wifi=" + this->settings.wifi().fingerprint();
    out += " eth=" + this->settings.ethernet().fingerprint();
    return out;
  }

  NetworkSettingsJson settings;
};

}  // namespace esphome::network_config::testing
