#pragma once
#include <gtest/gtest.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "esphome/components/config_json/config_json.h"
#include "esphome/components/dallas_scan/dallas_scan.h"
#include "esphome/components/dir_storage/dir_storage.h"
#include "esphome/components/host/core.h"
#include "esphome/components/host/preferences.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/one_wire_host/host_one_wire_bus.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

namespace esphome::dallas_scan::testing {

// DS18B20 ROMs: family 0x28 in the low byte, the way the bus reads them.
static const uint64_t ROM_A = 0xeb01227905460228ULL;
static const uint64_t ROM_B = 0x8a0122791699dd28ULL;
static const uint64_t ROM_C = 0x3c01b5566e8a1f28ULL;
// A 1-Wire device that is not a thermometer: a DS2401 serial number, family 0x01.
static const uint64_t ROM_SERIAL = 0x5f00001234567801ULL;

// The table a scan keeps, as the generated setup would have reached it.
inline void install_preferences() {
  [[maybe_unused]] static const bool ONCE = [] {
    setenv("ESPHOME_PREFDIR", ".prefs", 0);
    host::setup_preferences();
    return true;
  }();
}

// forget() ends in App.safe_reboot(), which exits the process with 0 on the host: a test that
// reboots by mistake would end the run as a pass. Armed, the reboot exec()s a path that is not
// there and exits with 1 instead. A case that expects the reboot disarms it in a death test.
inline void fail_on_reboot() { host::arm_reexec("/nonexistent/dallas-scan-test-rebooted"); }

// Every error, warning and dump_config() line the process logs. Registered once: the logger
// keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  std::vector<std::string> configs;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();  // NOLINT(cppcoreguidelines-owning-memory)
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() {
    this->errors.clear();
    this->warnings.clear();
    this->configs.clear();
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
    } else if (level == ESPHOME_LOG_LEVEL_CONFIG) {
      capture->configs.emplace_back(message, len);
    }
  }
};

// A YAML sensor for sensors:, which the scan only lists. Registered once, as codegen would have:
// App keeps the pointer for the life of the process.
inline sensor::Sensor &boiler() {
  static sensor::Sensor *instance = [] {
    auto *s = new sensor::Sensor();  // NOLINT(cppcoreguidelines-owning-memory)
    App.register_sensor(s, "Boiler", fnv1_hash("boiler"), 0);
    return s;
  }();
  return *instance;
}

// Listed sensors are set before boot(), as codegen sets them before setup().
using Listing = std::function<void(DallasScan &)>;

// A boot is a new scan over the same bus and the same storage: what it binds is what the table
// kept. Every case starts on a device whose preferences were never written, with an empty
// temporary folder standing for the user partition.
class Boots : public ::testing::Test {
 protected:
  static constexpr const char *KEY = "dallas_scan_temps";

  void SetUp() override {
    install_preferences();
    global_preferences->sync();
    global_preferences->reset();
    global_preferences->sync();
    fail_on_reboot();
    log().clear();
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->folder = folder;
  }

  void TearDown() override {
    chmod(this->dir().c_str(), 0755);
    chmod(this->file().c_str(), 0644);
    for (const std::string &name : this->files())
      remove((this->dir() + "/" + name).c_str());
    rmdir(this->dir().c_str());
    rmdir(this->folder.c_str());
  }

  // storage: file. Every boot gets a new keeper over the same folder, set up first as its
  // priority puts it, so the scan finds the config folder made. `mounted` false: the partition
  // did not mount, so the keeper's setup fails.
  DallasScan &boot(std::vector<uint64_t> devices, size_t max_sensors = 4, const Listing &listing = nullptr,
                   bool mounted = true) {
    auto storage = std::make_unique<dir_storage::DirStorage>();
    storage->set_base_path(this->folder);
    if (mounted)
      storage->setup();
    auto keeper = std::make_unique<config_json::ConfigJsonKeeper>();
    keeper->set_storage(storage.get());
    keeper->set_config_dir("config");
    DallasScan &scan = this->build(std::move(devices), max_sensors);
    scan.set_slot_file(keeper.get(), KEY);
    if (listing)
      listing(scan);
    keeper->setup();
    scan.setup();
    this->storages.push_back(std::move(storage));
    this->keepers.push_back(std::move(keeper));
    return scan;
  }

  // storage: nvs.
  DallasScan &boot_nvs(std::vector<uint64_t> devices, size_t max_sensors = 4, const Listing &listing = nullptr) {
    DallasScan &scan = this->build(std::move(devices), max_sensors);
    scan.set_preference_hash(fnv1_hash("temps"));
    if (listing)
      listing(scan);
    scan.setup();
    return scan;
  }

  // A scan as codegen builds one, up to where the storages differ.
  DallasScan &build(std::vector<uint64_t> devices, size_t max_sensors) {
    this->bus.set_devices(std::move(devices));
    auto scan = std::make_unique<DallasScan>();
    scan->set_one_wire_bus(&this->bus);
    scan->set_max_sensors(max_sensors);
    // Kept to the end of the case, so it can compare a boot with the one before it.
    this->boots.push_back(std::move(scan));
    return *this->boots.back();
  }

  std::string dir() const { return this->folder + "/config"; }
  std::string file() const { return this->dir() + "/" + KEY + ".json"; }
  void write(const std::string &text) {
    mkdir(this->dir().c_str(), 0755);
    std::ofstream out(this->file());
    out << text;
  }
  // "" when there is no file.
  std::string read() const {
    std::ifstream in(this->file());
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }
  std::vector<std::string> files() const {
    std::vector<std::string> names;
    DIR *d = opendir(this->dir().c_str());
    if (d == nullptr)
      return names;
    while (struct dirent *entry = readdir(d)) {
      std::string name = entry->d_name;
      if (name != "." && name != "..")
        names.push_back(name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    return names;
  }

  static LogCapture &log() { return LogCapture::instance(); }

  one_wire_host::HostOneWireBus bus;
  std::string folder;
  std::vector<std::unique_ptr<DallasScan>> boots;
  std::vector<std::unique_ptr<dir_storage::DirStorage>> storages;
  std::vector<std::unique_ptr<config_json::ConfigJsonKeeper>> keepers;
};

// The file a table is written as: {"slot": N, "address": "0x..."} for each bound slot.
inline std::string slot_file(const std::vector<std::pair<int, const char *>> &records) {
  std::string out = R"({"version":1,"records":[)";
  for (size_t i = 0; i < records.size(); i++) {
    if (i > 0)
      out += ",";
    out += R"({"slot":)" + std::to_string(records[i].first) + R"(,"address":")" + records[i].second + R"("})";
  }
  return out + "]}";
}

}  // namespace esphome::dallas_scan::testing
