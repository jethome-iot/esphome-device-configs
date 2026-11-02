// The week's clock: the local count of seconds, Monday-first minutes, HH:MM, and which point
// holds at a minute and which comes next.
#include "common.h"
#include <ctime>

namespace esphome::schedules::testing {

// timegm() is the host's own count of a civil date as UTC: the same arithmetic, independently.
static int64_t civil(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
  struct tm tm {};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  return static_cast<int64_t>(timegm(&tm));
}

TEST(WeekClock, DaysFromCivilCountsFromTheEpoch) {
  EXPECT_EQ(0, days_from_civil(1970, 1, 1));
  EXPECT_EQ(-1, days_from_civil(1969, 12, 31));
  for (int year : {2000, 2024, 2026, 2100}) {
    for (int month : {1, 2, 3, 12}) {
      EXPECT_EQ(civil(year, month, 1) / 86400, days_from_civil(year, month, 1)) << year << "-" << month;
    }
  }
  EXPECT_EQ(civil(2024, 2, 29) / 86400, days_from_civil(2024, 2, 29));
}

// The fields as they read, not the timestamp: a zone or DST change shows as a jump.
TEST(WeekClock, LocalSecondsCountTheFieldsNotTheTimestamp) {
  ESPTime time = ESPTime::from_epoch_utc(civil(2026, 10, 26, 6, 30, 15));
  EXPECT_EQ(civil(2026, 10, 26, 6, 30, 15), local_seconds(time));
  time.timestamp = 0;
  EXPECT_EQ(civil(2026, 10, 26, 6, 30, 15), local_seconds(time));
  time.hour = 7;
  EXPECT_EQ(civil(2026, 10, 26, 7, 30, 15), local_seconds(time));
}

TEST(WeekClock, TheWeekStartsOnMonday) {
  EXPECT_EQ(0, week_minute(ESPTime::from_epoch_utc(at_local(0, 0, 0))));
  EXPECT_EQ(wm(2, 10, 15), week_minute(ESPTime::from_epoch_utc(at_local(2, 10, 15, 42))));
  const ESPTime sunday = ESPTime::from_epoch_utc(at_local(6, 23, 59, 59));
  EXPECT_EQ(1, sunday.day_of_week) << "upstream counts Sunday as 1";
  EXPECT_EQ(6 * 1440 + 23 * 60 + 59, week_minute(sunday)) << "Sunday is the week's last day";
  EXPECT_EQ(0, week_minute(ESPTime::from_epoch_utc(at_local(7, 0, 0)))) << "the next Monday starts over";
}

// The engine counts the week from the local seconds alone; it must agree with the fields.
TEST(WeekClock, WeekMinuteOfAgreesWithTheFields) {
  for (int64_t local = at_local(-3, 0, 0); local < at_local(10, 0, 0); local += 37 * 60 + 11) {
    EXPECT_EQ(week_minute(ESPTime::from_epoch_utc(local)), week_minute_of(local / 60)) << local;
  }
}

TEST(WeekClock, HhmmIsTheTwentyFourHourClock) {
  uint16_t minutes = 7;
  EXPECT_TRUE(parse_hhmm("00:00", &minutes));
  EXPECT_EQ(0, minutes);
  EXPECT_TRUE(parse_hhmm("06:30", &minutes));
  EXPECT_EQ(390, minutes);
  EXPECT_TRUE(parse_hhmm("23:59", &minutes));
  EXPECT_EQ(1439, minutes);
  for (const char *text : {"24:00", "23:60", "7:00", "07:0", "07-00", "07:000", "", "ab:cd", " 7:00", "07:00 "}) {
    minutes = 7;
    EXPECT_FALSE(parse_hhmm(text, &minutes)) << text;
    EXPECT_EQ(7, minutes) << text;
  }
  EXPECT_EQ("00:00", format_hhmm(0));
  EXPECT_EQ("06:05", format_hhmm(365));
  EXPECT_EQ("23:59", format_hhmm(1439));
}

TEST(WeekClock, DayNamesAreMondayFirst) {
  EXPECT_STREQ("mon", DAY_NAMES[0]);
  EXPECT_STREQ("sun", DAY_NAMES[6]);
  EXPECT_STREQ("Mon", DAY_TITLES[0]);
  EXPECT_STREQ("Sun", DAY_TITLES[6]);
}

// --- Which point holds ---

static ScheduleConfig week_of(const std::vector<P> &points) { return relay_doc("Porch", "relay_1", points); }

TEST(InForce, BeforeMondaysFirstPointSundaysLastHolds) {
  const ScheduleConfig c = week_of({on_at(wm(0, 8, 0)), off_at(wm(4, 20, 0)), on_at(wm(6, 22, 0))});
  EXPECT_EQ(wm(6, 22, 0), c.in_force(wm(0, 0, 0))->minute_of_week);
  EXPECT_EQ(wm(6, 22, 0), c.in_force(wm(0, 7, 59))->minute_of_week);
  EXPECT_EQ(wm(0, 8, 0), c.in_force(wm(0, 8, 0))->minute_of_week);
}

TEST(InForce, SundayNightCarriesOverMidnightIntoMonday) {
  const ScheduleConfig c = week_of({on_at(wm(0, 0, 0)), off_at(wm(6, 23, 0))});
  EXPECT_EQ(wm(6, 23, 0), c.in_force(wm(6, 23, 59))->minute_of_week);
  EXPECT_EQ(wm(0, 0, 0), c.in_force(wm(0, 0, 0))->minute_of_week);
  EXPECT_EQ(wm(0, 0, 0), c.next_after(wm(6, 23, 59))->minute_of_week) << "the next point is next week's first";
}

TEST(InForce, ExactlyAtAPointThatPointHolds) {
  const ScheduleConfig c = week_of({on_at(wm(1, 6, 30)), off_at(wm(1, 6, 31))});
  EXPECT_EQ(wm(1, 6, 30), c.in_force(wm(1, 6, 30))->minute_of_week);
  EXPECT_EQ(wm(1, 6, 31), c.in_force(wm(1, 6, 31))->minute_of_week);
  EXPECT_EQ(wm(1, 6, 31), c.next_after(wm(1, 6, 30))->minute_of_week);
}

TEST(InForce, OnePointHoldsAllWeekAndComesNextItself) {
  const ScheduleConfig c = week_of({on_at(wm(3, 12, 0))});
  for (uint16_t minute : {wm(0, 0, 0), wm(3, 11, 59), wm(3, 12, 0), wm(6, 23, 59)}) {
    EXPECT_EQ(wm(3, 12, 0), c.in_force(minute)->minute_of_week) << minute;
    EXPECT_EQ(wm(3, 12, 0), c.next_after(minute)->minute_of_week) << minute;
  }
}

TEST(InForce, NextAfterIsTheNextPointEvenWithTheSameValue) {
  const ScheduleConfig c = week_of({on_at(wm(0, 8, 0)), on_at(wm(0, 9, 0)), off_at(wm(5, 20, 0))});
  EXPECT_EQ(wm(0, 9, 0), c.next_after(wm(0, 8, 0))->minute_of_week);
  EXPECT_EQ(wm(5, 20, 0), c.next_after(wm(0, 9, 0))->minute_of_week);
  EXPECT_EQ(wm(0, 8, 0), c.next_after(wm(5, 20, 0))->minute_of_week) << "round into next week";
}

TEST(InForce, NoPointsHoldNothing) {
  const ScheduleConfig c;
  EXPECT_EQ(nullptr, c.in_force(0));
  EXPECT_EQ(nullptr, c.next_after(0));
}

}  // namespace esphome::schedules::testing
