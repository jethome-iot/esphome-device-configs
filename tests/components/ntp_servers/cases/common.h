#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/host/preferences.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/ntp_servers/ntp_servers.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/application.h"
#include "esphome/core/preferences.h"

namespace esphome::ntp_servers::testing {

static const uint32_t PREF_HASH = 0x4E545053;
// sntp's default update_interval.
static const uint32_t POLL_MS = 15 * 60 * 1000;
// How long a server has to answer, as the component waits.
static const uint32_t ANSWER_MS = 30000;
// 2026-07-15 12:00 UTC.
static const time_t JULY = 1784116800;

static const char *const POOL_0 = "0.pool.ntp.org";
static const char *const POOL_1 = "1.pool.ntp.org";
static const char *const POOL_2 = "2.pool.ntp.org";

using Names = std::vector<std::string>;

// The generated setup would have installed the host backend before anything asked for a
// preference; main.cpp never runs it. The directory goes next to the binary, not into $HOME.
inline void install_preferences() {
  [[maybe_unused]] static const bool ONCE = [] {
    setenv("ESPHOME_PREFDIR", ".prefs", 0);
    host::setup_preferences();
    return true;
  }();
}

// What the component logged. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> lines;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() { this->lines.clear(); }
  bool has(const char *needle) const {
    return std::any_of(this->lines.begin(), this->lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t, const char *, const char *message, size_t len) {
    static_cast<LogCapture *>(self)->lines.emplace_back(message, len);
  }
};

// The SNTP clock: the case fires its sync as an answer from the server being asked.
class FakeClock : public time::RealTimeClock {
 public:
  void update() override {}
  void sync() { this->time_sync_callback_.call(); }
};

// The component with lwIP, the network, its instant and its flash in reach.
class TestServers : public NTPServers {
 public:
  // What the next store does: work, or fail the flush with the record either already in flash
  // -- another record's failure, seen from here -- or nowhere.
  enum class Flush { OK, FAILED_RECORD_LANDED, FAILED_RECORD_LOST };
  Flush flush{Flush::OK};
  int stores{0};
  time_t now{JULY};
  uint32_t ms{0};
  bool online{true};
  // Each server lwIP was handed, in order; "" for a stop.
  Names handed;

  const char *asked() const { return this->handed.empty() ? nullptr : this->handed.back().c_str(); }

 protected:
  void use_server_(const char *server) override { this->handed.emplace_back(server == nullptr ? "" : server); }
  bool connected_() const override { return this->online; }
  uint32_t millis_() const override { return this->ms; }
  time_t now_() const override { return this->now; }
  bool store_(const StoredServers &stored) override {
    this->stores++;
    if (this->flush != Flush::FAILED_RECORD_LOST)
      NTPServers::store_(stored);
    return this->flush == Flush::OK;
  }
};

// One boot of the device: its clock, which keeps a callback into the component, and the
// component.
struct Device {
  FakeClock clock;
  TestServers servers;
};

class Servers : public ::testing::Test {
 protected:
  void SetUp() override {
    install_preferences();
    LogCapture::instance().clear();
    // sync() first: it runs the backend's lazy read of the file, and a reset before that
    // would be undone by the next call that triggers it.
    global_preferences->sync();
    global_preferences->reset();
    global_preferences->sync();
  }

  // A device coming up with @p fallback as the clock's own servers and whatever was stored.
  TestServers &boot(const std::vector<const char *> &fallback = {POOL_0, POOL_1, POOL_2}, uint32_t ms = 0) {
    auto device = std::make_unique<Device>();
    device->clock.set_update_interval(POLL_MS);
    TestServers &servers = device->servers;
    servers.ms = ms;
    servers.set_clock(&device->clock);
    for (const char *server : fallback)
      servers.add_fallback_server(server);
    servers.set_preference_hash(PREF_HASH);
    servers.setup();
    this->devices.push_back(std::move(device));
    return servers;
  }
  Device &device() { return *this->devices.back(); }
  TestServers &servers() { return this->device().servers; }

  // The server being asked answers.
  void answer() { this->device().clock.sync(); }
  // @p ms go by, and then the loop task runs.
  void pass(uint32_t ms) {
    this->servers().ms += ms;
    this->servers().loop();
  }

  // A record as the preference holds it, written by another firmware.
  static void store_raw(const StoredServers &stored) {
    auto pref = global_preferences->make_preference<StoredServers>(PREF_HASH);
    ASSERT_TRUE(pref.save(&stored));
    ASSERT_TRUE(global_preferences->sync());
  }

  std::vector<std::unique_ptr<Device>> devices;
};

}  // namespace esphome::ntp_servers::testing
