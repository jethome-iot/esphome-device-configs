#pragma once
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#include "esphome/components/status_indicator/status_indicator.h"
#include "esphome/core/application.h"
#include "esphome/core/gpio.h"
#include "esphome/core/hal.h"

namespace esphome::status_indicator::testing {

// Every level written, with the millis() it was written at.
class RecordingPin : public GPIOPin {
 public:
  struct Write {
    uint32_t at;
    bool level;
  };
  std::vector<Write> writes;
  bool set_up{false};

  void setup() override { this->set_up = true; }
  void pin_mode(gpio::Flags) override {}
  gpio::Flags get_flags() const override { return gpio::FLAG_OUTPUT; }
  bool digital_read() override { return !this->writes.empty() && this->writes.back().level; }
  void digital_write(bool value) override { this->writes.push_back({millis(), value}); }
  size_t dump_summary(char *buffer, size_t len) const override { return snprintf(buffer, len, "recording pin"); }

  // The first n levels written, or all of them.
  std::vector<bool> levels(size_t n = SIZE_MAX) const {
    std::vector<bool> out;
    for (size_t i = 0; i < this->writes.size() && i < n; i++)
      out.push_back(this->writes[i].level);
    return out;
  }
  // Milliseconds from write i - 1 to write i.
  uint32_t gap(size_t i) const { return this->writes[i].at - this->writes[i - 1].at; }
};

// Distinct, so a gap names its phase, and long enough that a probe half way through the shortest
// sits FAST_ON / 2 from either end.
constexpr uint32_t FAST_ON = 150;
constexpr uint32_t FAST_OFF = 250;
constexpr uint32_t SLOW_ON = 400;
constexpr uint32_t SLOW_OFF = 600;
constexpr uint32_t PULSE = 500;
// The blink_n the cases ask for, apart from the fast and slow times.
constexpr uint32_t N_ON = 200;
constexpr uint32_t N_OFF = 300;
constexpr uint32_t N_PAUSE = 700;
// How late a phase may end: half the shortest one, far above the polling and the host's noise.
constexpr uint32_t SLACK = FAST_ON / 2;

class StatusIndicatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    this->new_indicator();
    this->led->setup();
    this->pin->writes.clear();
  }
  // Cancels a running timer: the state is OFF only when none is pending.
  void TearDown() override { this->led->turn_off(); }

  // Instances and pins outlive the test: a cancelled scheduler entry keeps pointing at its
  // component until a later call drops it.
  void new_indicator() {
    static std::vector<std::unique_ptr<RecordingPin>> pins;
    static std::vector<std::unique_ptr<StatusIndicator>> leds;
    pins.push_back(std::make_unique<RecordingPin>());
    leds.push_back(std::make_unique<StatusIndicator>());
    this->pin = pins.back().get();
    this->led = leds.back().get();
    this->led->set_pin(this->pin);
    this->led->set_slow_on_time(SLOW_ON);
    this->led->set_slow_off_time(SLOW_OFF);
    this->led->set_fast_on_time(FAST_ON);
    this->led->set_fast_off_time(FAST_OFF);
    this->led->set_pulse_duration(PULSE);
  }

  // The main loop for ms of wall clock: the only way a timeout fires in this harness.
  static void run_for(uint32_t ms) {
    const uint32_t start = millis();
    while (millis() - start < ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      App.scheduler.call(millis());
    }
  }

  // The main loop until the pin has seen `count` writes, so a late phase delays the check
  // instead of falling out of a fixed window. The cap only ends a test that would hang.
  void run_until_writes(size_t count) {
    const uint32_t start = millis();
    while (this->pin->writes.size() < count && millis() - start < 10000) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      App.scheduler.call(millis());
    }
  }

  // Runs until the pin has seen `levels`, then checks them in order, each `gaps[i - 1]` ms
  // after the last: never early, at most SLACK late.
  void expect_writes(const std::vector<bool> &levels, const std::vector<uint32_t> &gaps) {
    ASSERT_EQ(gaps.size() + 1, levels.size());
    this->run_until_writes(levels.size());
    ASSERT_GE(this->pin->writes.size(), levels.size());
    for (size_t i = 0; i < levels.size(); i++) {
      EXPECT_EQ(this->pin->writes[i].level, levels[i]) << "write " << i;
      if (i == 0)
        continue;
      EXPECT_GE(this->pin->gap(i), gaps[i - 1]) << "write " << i;
      EXPECT_LE(this->pin->gap(i), gaps[i - 1] + SLACK) << "write " << i;
    }
  }

  RecordingPin *pin{nullptr};
  StatusIndicator *led{nullptr};
};

}  // namespace esphome::status_indicator::testing
