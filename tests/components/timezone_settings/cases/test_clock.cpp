#include "common.h"

namespace esphome::timezone_settings::testing {

// 2026-07-15 13:00 UTC: an hour after the instant the fixture's clock reads.
static const time_t JULY_LATER = JULY + 3600;
// rtc-time.yaml's RTC update_interval, in milliseconds and in seconds.
static const uint32_t RTC_INTERVAL_MS = 15 * 60 * 1000;
static const time_t RTC_INTERVAL = RTC_INTERVAL_MS / 1000;

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

TEST_F(Timezone, AnRtcThatReadsBackTheSetTimeLeavesTheDashboard) {
  TestSettings &settings = this->boot();
  this->device().rtc.sync();
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  settings.ms += RTC_INTERVAL_MS;
  this->rtc_reads(JULY_LATER + RTC_INTERVAL);
  EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD);
  // Two seconds either way are the same time: the chip and the clock both count whole seconds.
  settings.ms += RTC_INTERVAL_MS;
  this->rtc_reads(JULY_LATER + 2 * RTC_INTERVAL + 2);
  EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD);
  settings.ms += RTC_INTERVAL_MS;
  this->rtc_reads(JULY_LATER + 3 * RTC_INTERVAL);
  EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD);
  EXPECT_FALSE(LogCapture::instance().has("The RTC reads"));
}

// The write on_clock_set made did not reach the RTC, so its next read puts the old time back.
TEST_F(Timezone, AnRtcThatPutsItsOwnTimeBackIsTheSource) {
  TestSettings &settings = this->boot();
  this->device().rtc.sync();
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  settings.ms += RTC_INTERVAL_MS;
  this->rtc_reads(JULY + RTC_INTERVAL);
  EXPECT_EQ(settings.clock_source(), ClockSource::RTC);
  EXPECT_TRUE(LogCapture::instance().has("The RTC reads -3600 s off the time dashboard set"));
  settings.ms += RTC_INTERVAL_MS;
  this->rtc_reads(JULY + 2 * RTC_INTERVAL);
  EXPECT_EQ(settings.clock_source(), ClockSource::RTC);
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD);
}

TEST_F(Timezone, ThreeSecondsOffIsAnotherTime) {
  for (const time_t off : {time_t{-3}, time_t{3}}) {
    TestSettings &settings = this->boot();
    ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
    settings.ms += RTC_INTERVAL_MS;
    this->rtc_reads(JULY_LATER + RTC_INTERVAL + off);
    EXPECT_EQ(settings.clock_source(), ClockSource::RTC) << off;
  }
}

// About 49.7 days up, millis() starts over from 0 between the set and the read.
TEST_F(Timezone, AMillisWrapBetweenTheSetAndTheReadCountsRight) {
  TestSettings &settings = this->boot();
  settings.ms = UINT32_MAX - 1000;
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  settings.ms += RTC_INTERVAL_MS;
  ASSERT_LT(settings.ms, RTC_INTERVAL_MS);
  this->rtc_reads(JULY_LATER + RTC_INTERVAL);
  EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD);

  TestSettings &failed = this->boot();
  failed.ms = UINT32_MAX - 1000;
  ASSERT_EQ(failed.set_clock(JULY_LATER), SetError::NONE);
  failed.ms += RTC_INTERVAL_MS;
  this->rtc_reads(JULY + RTC_INTERVAL);
  EXPECT_EQ(failed.clock_source(), ClockSource::RTC);
}

// The RTC's crystal and the one millis() counts drift apart; a second per read never adds up.
TEST_F(Timezone, AnRtcDriftingASecondEachReadStaysTheSetTime) {
  TestSettings &settings = this->boot();
  ASSERT_EQ(settings.set_clock(JULY_LATER), SetError::NONE);
  time_t rtc = JULY_LATER;
  for (int read = 0; read < 10; read++) {
    settings.ms += RTC_INTERVAL_MS;
    rtc += RTC_INTERVAL + 1;
    this->rtc_reads(rtc);
    EXPECT_EQ(settings.clock_source(), ClockSource::DASHBOARD) << read;
  }
}

// Home Assistant's and SNTP's syncs write the RTC too, and that write can fail the same way.
TEST_F(Timezone, AnRtcThatPutsItsOwnTimeBackAfterTheNetworkIsTheSource) {
  TestSettings &settings = this->boot();
  for (const ClockSource source : {ClockSource::SNTP, ClockSource::HOMEASSISTANT}) {
    FakeClock &clock = source == ClockSource::SNTP ? this->device().sntp : this->device().homeassistant;
    settings.now = JULY;
    clock.sync();
    ASSERT_EQ(settings.clock_source(), source);
    settings.ms += RTC_INTERVAL_MS;
    this->rtc_reads(JULY + RTC_INTERVAL);
    EXPECT_EQ(settings.clock_source(), source) << clock_source_name(source);
    // That sync's write failed: the RTC goes on from the time it held before.
    settings.now = JULY_LATER;
    clock.sync();
    settings.ms += RTC_INTERVAL_MS;
    this->rtc_reads(JULY + 2 * RTC_INTERVAL);
    EXPECT_EQ(settings.clock_source(), ClockSource::RTC) << clock_source_name(source);
    this->loop();
  }
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
