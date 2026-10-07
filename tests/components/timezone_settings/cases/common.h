#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/host/preferences.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/timezone_settings/posix_tz_parse.h"
#include "esphome/components/timezone_settings/timezone_settings.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"

namespace esphome::timezone_settings::testing {

static const uint32_t PREF_HASH = 0x7153E771;

// POSIX strings as tzdata gives them.
static const char *const BERLIN = "CET-1CEST,M3.5.0,M10.5.0/3";
static const char *const NEW_YORK = "EST5EDT,M3.2.0,M11.1.0";
static const char *const TOKYO = "JST-9";
static const char *const SYDNEY = "AEST-10AEDT,M10.1.0,M4.1.0/3";
static const char *const KOLKATA = "IST-5:30";
static const char *const ST_JOHNS = "NST3:30NDT,M3.2.0,M11.1.0";
static const char *const KATHMANDU = "<+0545>-5:45";

// 2026-01-15 and 2026-07-15, 12:00 UTC: winter and summer north of the equator.
static const time_t JANUARY = 1768478400;
static const time_t JULY = 1784116800;

inline time::ParsedTimezone parsed(const char *posix) {
  time::ParsedTimezone tz{};
  EXPECT_TRUE(parse_posix_tz(posix, tz)) << posix;
  return tz;
}

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

// A clock whose sync the case fires, as Home Assistant's answer, an SNTP reply or an RTC read
// would.
class FakeClock : public time::RealTimeClock {
 public:
  void update() override {}
  void sync() { this->time_sync_callback_.call(); }
};

// The component with its instant and its flash in reach.
class TestSettings : public TimezoneSettings {
 public:
  // What the next store does: work, or fail the flush with the record either already in flash
  // -- another record's failure, seen from here -- or nowhere.
  enum class Flush { OK, FAILED_RECORD_LANDED, FAILED_RECORD_LOST };
  Flush flush{Flush::OK};
  int stores{0};
  time_t now{JULY};

 protected:
  time_t now_() const override { return this->now; }
  bool store_(const StoredSettings &stored) override {
    this->stores++;
    if (this->flush != Flush::FAILED_RECORD_LOST)
      TimezoneSettings::store_(stored);
    return this->flush == Flush::OK;
  }
};

// The chosen zone, in effect at once.
inline SetError stop_following(TimezoneSettings &settings, const Zone &zone) {
  return settings.configure(false, &zone);
}

// One boot of the device: its clocks, which keep a callback into the component, and the
// component. Kept by the fixture until the scheduler has run what they deferred.
struct Device {
  FakeClock homeassistant;
  FakeClock sntp;
  FakeClock rtc;
  TestSettings settings;
};

class Timezone : public ::testing::Test {
 protected:
  void SetUp() override {
    install_preferences();
    LogCapture::instance().clear();
    // sync() first: it runs the backend's lazy read of the file, and a reset before that
    // would be undone by the next call that triggers it.
    global_preferences->sync();
    global_preferences->reset();
    global_preferences->sync();
    this->homeassistant_zone = time::ParsedTimezone{};
    time::set_global_tz(time::ParsedTimezone{});
  }

  void TearDown() override { this->loop(); }

  // What the loop task does next: run what was deferred.
  static void loop() { App.scheduler.call(millis()); }

  // A device coming up on the compiled @p posix, with Home Assistant's zone as the global
  // restored it and whatever the component stored.
  TestSettings &boot(const char *zone = "Europe/Berlin", const char *posix = BERLIN, uint32_t hash = PREF_HASH) {
    auto device = std::make_unique<Device>();
    TestSettings &settings = device->settings;
    settings.set_default_zone(zone, posix);
    settings.set_homeassistant_zone(&this->homeassistant_zone);
    settings.set_preference_hash(hash);
    settings.add_clock(&device->homeassistant, ClockSource::HOMEASSISTANT);
    settings.add_clock(&device->sntp, ClockSource::SNTP);
    settings.add_clock(&device->rtc, ClockSource::RTC);
    settings.setup();
    this->devices.push_back(std::move(device));
    return settings;
  }
  Device &device() { return *this->devices.back(); }

  // Home Assistant's answer as api_connection.cpp handles it: the sync callbacks first, then
  // the zone the message carries, if it carries one.
  void homeassistant_answers(const time::ParsedTimezone *zone) {
    this->device().homeassistant.sync();
    if (zone != nullptr)
      time::set_global_tz(*zone);
  }

  static bool in_effect(const time::ParsedTimezone &zone) { return same_zone(time::get_global_tz(), zone); }

  // The restoring global's value, as it would come back from flash.
  time::ParsedTimezone homeassistant_zone{};
  std::vector<std::unique_ptr<Device>> devices;
};

}  // namespace esphome::timezone_settings::testing
