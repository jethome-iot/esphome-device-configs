#include "posix_tz_parse.h"
#include <cstdint>
#include <limits>

// A port of aioesphomeapi/posix_tz.py, step for step, so the two agree on every string; the
// suite holds them to that over every zone in tzdata.
namespace esphome::timezone_settings {

using time::DSTRule;
using time::DSTRuleType;
using time::ParsedTimezone;

// ASCII only: Python's isalpha() and isdigit() also take other scripts' letters and digits,
// which parse_posix_tz() refuses before it gets here.
static bool is_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_sign(char c) { return c == '+' || c == '-'; }

static bool fits_int32(int64_t value) {
  return value >= std::numeric_limits<int32_t>::min() && value <= std::numeric_limits<int32_t>::max();
}

// _skip_tz_name: <anything> or three letters or more.
static bool skip_name(const std::string &s, size_t &pos) {
  if (s[pos] == '<') {
    const size_t close = s.find('>', pos + 1);
    if (close == std::string::npos)
      return false;
    pos = close + 1;
    return true;
  }
  const size_t start = pos;
  while (pos < s.size() && is_letter(s[pos]))
    pos++;
  return pos - start >= 3;
}

// _parse_uint: at least one digit. Python's int has no ceiling; past 32 bits nothing in the
// struct could hold the value anyway.
static bool parse_uint(const std::string &s, size_t &pos, uint32_t &value) {
  const size_t start = pos;
  uint64_t total = 0;
  while (pos < s.size() && is_digit(s[pos])) {
    total = total * 10 + static_cast<uint64_t>(s[pos] - '0');
    if (total > std::numeric_limits<uint32_t>::max())
      return false;
    pos++;
  }
  if (pos == start)
    return false;
  value = static_cast<uint32_t>(total);
  return true;
}

// _parse_offset: [+|-]hh[:mm[:ss]] in seconds.
static bool parse_offset(const std::string &s, size_t &pos, int32_t &seconds) {
  int64_t sign = 1;
  if (pos < s.size() && s[pos] == '-') {
    sign = -1;
    pos++;
  } else if (pos < s.size() && s[pos] == '+') {
    pos++;
  }
  uint32_t hours;
  uint32_t minutes = 0;
  uint32_t secs = 0;
  if (!parse_uint(s, pos, hours))
    return false;
  if (pos < s.size() && s[pos] == ':') {
    pos++;
    if (!parse_uint(s, pos, minutes))
      return false;
    if (pos < s.size() && s[pos] == ':') {
      pos++;
      if (!parse_uint(s, pos, secs))
        return false;
    }
  }
  const int64_t total = sign * (static_cast<int64_t>(hours) * 3600 + static_cast<int64_t>(minutes) * 60 + secs);
  if (!fits_int32(total))
    return false;
  seconds = static_cast<int32_t>(total);
  return true;
}

// The '.' between the fields of an Mm.w.d rule.
static bool skip_dot(const std::string &s, size_t &pos) {
  if (pos >= s.size() || s[pos] != '.')
    return false;
  pos++;
  return true;
}

// _parse_dst_rule: Mm.w.d, Jn or n, then an optional /time.
static bool parse_rule(const std::string &s, size_t &pos, DSTRule &rule) {
  if (pos >= s.size())
    return false;
  uint32_t value;
  const char kind = s[pos];
  if (kind == 'M' || kind == 'm') {
    rule.type = DSTRuleType::MONTH_WEEK_DAY;
    pos++;
    if (!parse_uint(s, pos, value) || value < 1 || value > 12)
      return false;
    rule.month = static_cast<uint8_t>(value);
    if (!skip_dot(s, pos) || !parse_uint(s, pos, value) || value < 1 || value > 5)
      return false;
    rule.week = static_cast<uint8_t>(value);
    if (!skip_dot(s, pos) || !parse_uint(s, pos, value) || value > 6)
      return false;
    rule.day_of_week = static_cast<uint8_t>(value);
  } else if (kind == 'J' || kind == 'j') {
    rule.type = DSTRuleType::JULIAN_NO_LEAP;
    pos++;
    if (!parse_uint(s, pos, value) || value < 1 || value > 365)
      return false;
    rule.day = static_cast<uint16_t>(value);
  } else if (is_digit(kind)) {
    rule.type = DSTRuleType::DAY_OF_YEAR;
    if (!parse_uint(s, pos, value) || value > 365)
      return false;
    rule.day = static_cast<uint16_t>(value);
  } else {
    return false;
  }
  rule.time_seconds = 2 * 3600;
  if (pos < s.size() && s[pos] == '/') {
    pos++;
    return parse_offset(s, pos, rule.time_seconds);
  }
  return true;
}

// parse_posix_tz itself, including where it stops early and ignores the rest.
static bool parse(const std::string &s, ParsedTimezone &tz) {
  size_t pos = 0;
  if (!skip_name(s, pos))
    return false;
  if (pos >= s.size() || (!is_digit(s[pos]) && !is_sign(s[pos])))
    return false;
  if (!parse_offset(s, pos, tz.std_offset_seconds))
    return false;
  if (pos >= s.size())
    return true;
  if (s[pos] == ',')
    return false;
  if (!is_letter(s[pos]) && s[pos] != '<')
    return true;
  if (!skip_name(s, pos))
    return false;
  if (pos < s.size() && (is_digit(s[pos]) || is_sign(s[pos]))) {
    if (!parse_offset(s, pos, tz.dst_offset_seconds))
      return false;
  } else {
    const int64_t dst = static_cast<int64_t>(tz.std_offset_seconds) - 3600;
    if (!fits_int32(dst))
      return false;
    tz.dst_offset_seconds = static_cast<int32_t>(dst);
  }
  // A DST name without rules is no DST, though the DST offset stays set.
  if (pos >= s.size() || s[pos] != ',')
    return true;
  pos++;
  if (!parse_rule(s, pos, tz.dst_start))
    return false;
  if (pos >= s.size() || s[pos] != ',')
    return false;
  pos++;
  return parse_rule(s, pos, tz.dst_end);
}

bool parse_posix_tz(const std::string &text, ParsedTimezone &out) {
  if (text.empty())
    return false;
  for (const char c : text) {
    if (c < 0x20 || c > 0x7E)
      return false;
  }
  ParsedTimezone tz{};
  if (!parse(text, tz))
    return false;
  out = tz;
  return true;
}

}  // namespace esphome::timezone_settings
