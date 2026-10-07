#include "common.h"

namespace esphome::timezone_settings::testing {

// What GET /api/device/time reports: east of UTC is positive, the opposite of POSIX.
TEST_F(Timezone, TheOffsetIsEastOfUtcWithDstIncluded) {
  struct Expected {
    const char *posix;
    time_t at;
    int64_t utc_offset;
    bool dst;
  };
  for (const Expected &e : {
           Expected{BERLIN, JANUARY, 3600, false},
           Expected{BERLIN, JULY, 7200, true},
           Expected{NEW_YORK, JANUARY, -18000, false},
           Expected{NEW_YORK, JULY, -14400, true},
           Expected{SYDNEY, JANUARY, 39600, true},  // south of the equator, summer in January
           Expected{SYDNEY, JULY, 36000, false},
           Expected{KOLKATA, JULY, 19800, false},
           Expected{ST_JOHNS, JANUARY, -12600, false},
           Expected{"UTC0", JULY, 0, false},
       }) {
    TestSettings &settings = this->boot("Test", e.posix);
    settings.now = e.at;
    const TimezoneSettings::Status status = settings.status();
    ASSERT_TRUE(status.time.has_value()) << e.posix;
    EXPECT_EQ(status.time.value(), e.at) << e.posix;
    EXPECT_EQ(status.utc_offset, e.utc_offset) << e.posix << " at " << e.at;
    EXPECT_EQ(status.dst, e.dst) << e.posix << " at " << e.at;
  }
}

// A clock that was never set reads 1970: no time, and the standard offset.
TEST_F(Timezone, WithoutTheTimeTheStandardOffsetIsReported) {
  TestSettings &settings = this->boot();
  for (const time_t unset : {static_cast<time_t>(0), static_cast<time_t>(1546300799)}) {
    settings.now = unset;
    const TimezoneSettings::Status status = settings.status();
    EXPECT_FALSE(status.time.has_value());
    EXPECT_EQ(status.utc_offset, 3600);
    EXPECT_FALSE(status.dst);
  }
  settings.now = 1546300800;  // 2019-01-01, the first instant a clock counts as set
  EXPECT_TRUE(settings.status().time.has_value());
}

TEST_F(Timezone, TheOffsetFollowsTheZoneInEffect) {
  TestSettings &settings = this->boot();
  settings.now = JANUARY;
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  EXPECT_EQ(settings.status().utc_offset, 32400);
  settings.set_follow_homeassistant(true);
  EXPECT_EQ(settings.status().utc_offset, 3600);
}

TEST_F(Timezone, TheOffsetOfTheWestmostZoneDoesNotOverflow) {
  TestSettings &settings = this->boot("Test", "EST-596523:14:08");
  settings.now = 0;
  EXPECT_EQ(settings.status().utc_offset, 2147483648LL);
}

TEST_F(Timezone, ThePanelShowsTheChosenZonesCity) {
  TestSettings &settings = this->boot();
  stop_following(settings, Zone{"America/Argentina/Buenos_Aires", "<-03>3"});
  EXPECT_EQ(settings.zone_label(), "Buenos Aires");
  stop_following(settings, Zone{"UTC", "UTC0"});
  EXPECT_EQ(settings.zone_label(), "UTC");
  stop_following(settings, Zone{"Etc/GMT+3", "<-03>3"});
  EXPECT_EQ(settings.zone_label(), "GMT+3");
}

TEST_F(Timezone, ThePanelShowsTheOffsetOtherwise) {
  TestSettings &settings = this->boot();
  settings.now = JULY;
  EXPECT_EQ(settings.zone_label(), "UTC+2");  // the compiled Berlin, in summer
  settings.now = JANUARY;
  EXPECT_EQ(settings.zone_label(), "UTC+1");

  // A chosen zone with no name, or one ending in a slash, has no city to show.
  stop_following(settings, Zone{"", KOLKATA});
  EXPECT_EQ(settings.zone_label(), "UTC+5:30");
  stop_following(settings, Zone{"Somewhere/", ST_JOHNS});
  EXPECT_EQ(settings.zone_label(), "UTC-3:30");

  // A city chosen but not followed is not what the clock reads.
  settings.set_zone(Zone{"Asia/Kathmandu", KATHMANDU});
  settings.set_follow_homeassistant(true);
  EXPECT_EQ(settings.zone_label(), "UTC+1");
}

TEST(OffsetText, ReadsAsTheOffsetFromUtc) {
  EXPECT_EQ(utc_offset_text(0), "UTC");
  EXPECT_EQ(utc_offset_text(3 * 3600), "UTC+3");
  EXPECT_EQ(utc_offset_text(-5 * 3600), "UTC-5");
  EXPECT_EQ(utc_offset_text(5 * 3600 + 30 * 60), "UTC+5:30");
  EXPECT_EQ(utc_offset_text(-(3 * 3600 + 30 * 60)), "UTC-3:30");
  EXPECT_EQ(utc_offset_text(5 * 3600 + 45 * 60), "UTC+5:45");
  EXPECT_EQ(utc_offset_text(14 * 3600), "UTC+14");
  EXPECT_EQ(utc_offset_text(5 * 3600 + 45 * 60 + 30), "UTC+5:45:30");
  EXPECT_EQ(utc_offset_text(-30), "UTC-0:00:30");
  EXPECT_EQ(utc_offset_text(2147483648LL), "UTC+596523:14:08");
  EXPECT_EQ(utc_offset_text(-2147483647LL), "UTC-596523:14:07");
}

TEST(ZoneCity, IsTheLastSegmentWithSpaces) {
  EXPECT_EQ(zone_city("Europe/Berlin"), "Berlin");
  EXPECT_EQ(zone_city("America/North_Dakota/New_Salem"), "New Salem");
  EXPECT_EQ(zone_city("America/Port-au-Prince"), "Port-au-Prince");
  EXPECT_EQ(zone_city("UTC"), "UTC");
  EXPECT_EQ(zone_city("Somewhere/"), "");
  EXPECT_EQ(zone_city(""), "");
}

}  // namespace esphome::timezone_settings::testing
