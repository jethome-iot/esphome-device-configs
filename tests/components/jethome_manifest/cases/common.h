#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>
#include "esphome/components/jethome_manifest/jethome_manifest.h"
#include "esphome/components/logger/logger.h"

namespace esphome::jethome_manifest::testing {

// Where the manifests in these tests came from; the relative URLs inside them resolve against it.
static const char *const SOURCE = "https://fw.jethome.com/api/devices/jxd-r6-e1eth-lcd/info";

// The server's answer, trimmed to the fields the component reads plus the ones it steps over.
static const char *const MANIFEST = R"({
  "vendor": "jethome",
  "device": "jxd-r6-e1eth-lcd",
  "device_name": "JXD-R6-E1ETH-LCD",
  "latest_firmware": {
    "firmware.esphome.jxd-r6-e1eth-lcd.release": {
      "version": "2026.8.2.0",
      "images": {
        "esp.bin": {"url": "/media/release-factory.bin", "hash": "53f558a8cd0020eb86d4bf4f8fbe9511"},
        "esp.ota": {"url": "/media/release-ota.bin", "hash": "8fd271ebf20c980fac0168646e8fa600"}
      },
      "changelog": "/devices/jethome/jxd-r6-e1eth-lcd/fw/release/CHANGELOG.md"
    },
    "firmware.esphome.jxd-r6-e1eth-lcd.nightly": {
      "version": "2026.8.2.20260917.1",
      "images": {
        "esp.ota": {"url": "/media/nightly-ota.bin", "hash": "448059cb4a86188fa325956e3b69f09f"}
      }
    }
  }
})";

// Every error logged since clear(). Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> errors;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() { this->errors.clear(); }
  bool has_error(const char *needle) const {
    return std::any_of(this->errors.begin(), this->errors.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    if (level == ESPHOME_LOG_LEVEL_ERROR)
      static_cast<LogCapture *>(self)->errors.emplace_back(message, len);
  }
};

inline bool parse(const std::string &manifest, const std::string &channel, update::UpdateInfo &info,
                  const char *source = SOURCE) {
  LogCapture::instance().clear();
  return parse_manifest(reinterpret_cast<const uint8_t *>(manifest.data()), manifest.size(), channel, source, info);
}

inline const LogCapture &log() { return LogCapture::instance(); }

}  // namespace esphome::jethome_manifest::testing
