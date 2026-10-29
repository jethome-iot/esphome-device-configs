#pragma once

#include <cstdint>
#include <string>
#include "esphome/core/time.h"

namespace esphome::schedules {

static constexpr uint16_t MINUTES_PER_DAY = 1440;
static constexpr uint16_t MINUTES_PER_WEEK = 7 * MINUTES_PER_DAY;

/// The days as the file and the API name them, Monday first: index 0 is Monday.
extern const char *const DAY_NAMES[7];
/// The same as a sentence starts with them: "Mon".
extern const char *const DAY_TITLES[7];

/// Days from 1970-01-01 to a date of the proleptic Gregorian calendar.
int64_t days_from_civil(int64_t year, unsigned month, unsigned day);
/// The date and time `time`'s fields say, as seconds from 1970-01-01 00:00 on that same clock.
/// Not `timestamp`: a time zone change or a DST switch moves this, and is seen as a jump.
int64_t local_seconds(const ESPTime &time);
/// Minutes since Monday 00:00, from `time`'s own day_of_week (Sunday is 6).
uint16_t week_minute(const ESPTime &time);
/// Minutes since Monday 00:00 of local_seconds() / 60.
uint16_t week_minute_of(int64_t local_minute);

/// "HH:MM" on the 24-hour clock, 00:00 to 23:59, into minutes since midnight.
bool parse_hhmm(const char *text, uint16_t *minutes);
/// Minutes since midnight as "HH:MM".
std::string format_hhmm(uint16_t minutes);

}  // namespace esphome::schedules
