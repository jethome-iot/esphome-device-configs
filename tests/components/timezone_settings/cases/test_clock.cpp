#include "common.h"

namespace esphome::timezone_settings::testing {

// 2026-07-15 13:00 UTC: an hour after the instant the fixture's clock reads.
static const time_t JULY_LATER = JULY + 3600;

TEST_F(Timezone, SettingTheClockPutsThatEpochOnIt) {
  TestSettings &settings = this->boot();
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  EXPECT_EQ(settings.clock_sets, std::vector<uint32_t>{JULY_LATER});
  EXPECT_EQ(settings.status().time, optional<time_t>(JULY_LATER));
  EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD);
  EXPECT_TRUE(LogCapture::instance().has("Clock set by hand"));
}

TEST_F(Timezone, OnClockSetRunsOnceAfterTheClockIsSet) {
  TestSettings &settings = this->boot();
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  const auto &runs = this->device().clock_set_runs;
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].now, JULY_LATER);
  EXPECT_EQ(runs[0].source, ClockSource::DASHBOARD);
  // The same instant again changes nothing on the clock, but the RTC is written all the same.
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  EXPECT_EQ(runs.size(), 2u);
  EXPECT_EQ(settings.clock_sets.size(), 2u);
}

TEST_F(Timezone, TheClockTakes2019To2099) {
  struct Case {
    int64_t epoch;
    SetError error;
  };
  for (const Case &c : {
           Case{1546300799, SetError::TIME_OUT_OF_RANGE},
           Case{1546300800, SetError::NONE},  // 2019-01-01T00:00:00Z
           Case{4102444799, SetError::NONE},  // 2099-12-31T23:59:59Z
           Case{4102444800, SetError::TIME_OUT_OF_RANGE},
           Case{0, SetError::TIME_OUT_OF_RANGE},
           Case{-1, SetError::TIME_OUT_OF_RANGE},
           Case{INT64_MIN, SetError::TIME_OUT_OF_RANGE},
           Case{INT64_MAX, SetError::TIME_OUT_OF_RANGE},
           Case{4102444800 + (int64_t{1} << 32), SetError::TIME_OUT_OF_RANGE},
       }) {
    EXPECT_EQ(TimezoneSettings::check_clock(c.epoch), c.error) << c.epoch;
  }
}

TEST_F(Timezone, ATimeOutOfRangeTouchesNothing) {
  TestSettings &settings = this->boot();
  this->device().sntp.sync();
  for (const int64_t epoch : {int64_t{1546300799}, int64_t{4102444800}, int64_t{-1}}) {
    EXPECT_EQ(settings.set_clock(epoch), SetError::TIME_OUT_OF_RANGE) << epoch;
  }
  EXPECT_TRUE(settings.clock_sets.empty());
  EXPECT_TRUE(this->device().clock_set_runs.empty());
  EXPECT_EQ(settings.clock_source(), ClockSource::SNTP);
  EXPECT_EQ(settings.status().time, optional<time_t>(JULY));
}

// Both ends reach the clock as they were sent: the last second of 2099 still fits 32 bits.
TEST_F(Timezone, TheEdgesOfTheRangeReachTheClockWhole) {
  TestSettings &settings = this->boot();
  ASSERT_EQ(settings.set_clock(1546300800), SetError::NONE);
  ASSERT_EQ(settings.set_clock(4102444799), SetError::NONE);
  EXPECT_EQ(settings.clock_sets, (std::vector<uint32_t>{1546300800u, 4102444799u}));
  EXPECT_EQ(settings.status().time, optional<time_t>(4102444799));
}

TEST_F(Timezone, AClockThatDoesNotTakeTheTimeIsAnError) {
  TestSettings &settings = this->boot();
  this->device().rtc.sync();
  settings.clock_takes = false;
  EXPECT_EQ(settings.set_clock(JULY_LATER), SetError::CLOCK_NOT_SET);
  EXPECT_EQ(settings.clock_sets, std::vector<uint32_t>{JULY_LATER});
  EXPECT_TRUE(LogCapture::instance().has("The system clock did not take 1784120400"));
  EXPECT_EQ(settings.clock_source(), ClockSource::RTC);
  EXPECT_TRUE(this->device().clock_set_runs.empty());
}

TEST_F(Timezone, TheDashboardIsTheSourceUntilANetworkClockSyncs) {
  TestSettings &settings = this->boot();
  this->device().rtc.sync();
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD);
  // The RTC's periodic re-read reads back what on_clock_set wrote into it.
  this->device().rtc.sync();
  EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD);
  this->device().sntp.sync();
  EXPECT_EQ(settings.clock_source(), ClockSource::SNTP);
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD);
  this->device().homeassistant.sync();
  EXPECT_EQ(settings.clock_source(), ClockSource::HOMEASSISTANT);
  this->loop();
}

TEST_F(Timezone, ASetClockIsForgottenAtTheReboot) {
  ASSERT_EQ(this->boot().set_clock(JULY_LATER), SetError::NONE);
  TestSettings &rebooted = this->boot();
  EXPECT_EQ(rebooted.clock_source(), ClockSource::NONE);
  this->device().rtc.sync();
  EXPECT_EQ(rebooted.clock_source(), ClockSource::RTC);
}

TEST_F(Timezone, SettingTheClockLeavesTheZoneAlone) {
  TestSettings &settings = this->boot();
  ASSERT_EQ(stop_following(settings, Zone{"Asia/Tokyo", TOKYO}), SetError::NONE);
  const int stores = settings.stores;
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));
  EXPECT_EQ(settings.zone_source(), ZoneSource::MANUAL);
  EXPECT_EQ(settings.stores, stores);
}

// Without an automation the trigger has nowhere to go; the clock is set all the same.
TEST_F(Timezone, NoOnClockSetIsNoProblem) {
  TestSettings settings;
  EXPECT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD);
}

// Upstream's synchronize_epoch_ leaves a clock within a second of the epoch alone, so the real
// step runs here without moving the host's clock; any other epoch would.
TEST_F(Timezone, TheRealClockTakesTheTimeItAlreadyShows) {
  TestSettings &settings = this->boot();
  EXPECT_TRUE(settings.set_host_clock(static_cast<uint32_t>(::time(nullptr))));
}

}  // namespace esphome::timezone_settings::testing
