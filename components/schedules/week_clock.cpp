#include "week_clock.h"
#include <cstdio>
#include <cstring>

namespace esphome::schedules {

const char *const DAY_NAMES[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
const char *const DAY_TITLES[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

// 1970-01-05, the first Monday after the epoch.
static constexpr int64_t MONDAY_EPOCH_MINUTE = 4 * MINUTES_PER_DAY;

static int64_t floor_mod(int64_t value, int64_t divisor) {
  const int64_t rest = value % divisor;
  return rest < 0 ? rest + divisor : rest;
}

// Howard Hinnant's days_from_civil: exact for every year, no table, no time zone.
int64_t days_from_civil(int64_t year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t local_seconds(const ESPTime &time) {
  return days_from_civil(time.year, time.month, time.day_of_month) * 86400 + time.hour * 3600 + time.minute * 60 +
         time.second;
}

uint16_t week_minute(const ESPTime &time) {
  return static_cast<uint16_t>(((time.day_of_week + 5) % 7) * MINUTES_PER_DAY + time.hour * 60 + time.minute);
}

uint16_t week_minute_of(int64_t local_minute) {
  return static_cast<uint16_t>(floor_mod(local_minute - MONDAY_EPOCH_MINUTE, MINUTES_PER_WEEK));
}

bool parse_hhmm(const char *text, uint16_t *minutes) {
  auto digit = [text](size_t i) { return text[i] >= '0' && text[i] <= '9'; };
  if (strlen(text) != 5 || !digit(0) || !digit(1) || text[2] != ':' || !digit(3) || !digit(4))
    return false;
  const unsigned hour = (text[0] - '0') * 10 + (text[1] - '0');
  const unsigned minute = (text[3] - '0') * 10 + (text[4] - '0');
  if (hour > 23 || minute > 59)
    return false;
  *minutes = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

std::string format_hhmm(uint16_t minutes) {
  char text[8];
  snprintf(text, sizeof(text), "%02u:%02u", (minutes / 60u) % 100u, minutes % 60u);
  return text;
}

}  // namespace esphome::schedules
