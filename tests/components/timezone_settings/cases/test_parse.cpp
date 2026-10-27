#include <cstdint>
#include <fstream>
#include <sstream>
#include "common.h"

namespace esphome::timezone_settings::testing {

// generate_fixture.py's output, next to test.yaml: run.py runs the suite from there.
static const char *const FIXTURE = "posix_tz_cases.tsv";

struct Case {
  std::string kind;
  std::string label;
  std::string posix;
  bool ok{false};
  std::vector<long long> fields;
};

static std::vector<Case> read_fixture() {
  std::vector<Case> cases;
  std::ifstream in(FIXTURE);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::vector<std::string> columns;
    std::stringstream stream(line);
    std::string column;
    while (std::getline(stream, column, '\t'))
      columns.push_back(column);
    Case c;
    c.kind = columns.at(0);
    c.label = columns.at(1);
    c.posix = columns.at(2);
    c.ok = columns.at(3) == "ok";
    for (size_t i = 4; i < columns.size(); i++)
      c.fields.push_back(std::stoll(columns[i]));
    cases.push_back(c);
  }
  return cases;
}

static std::vector<long long> rule_fields(const time::DSTRule &rule) {
  return {static_cast<long long>(rule.type),        rule.month, rule.week, rule.day_of_week, rule.day,
          static_cast<long long>(rule.time_seconds)};
}

static std::vector<long long> fields_of(const time::ParsedTimezone &tz) {
  std::vector<long long> out{tz.std_offset_seconds, tz.dst_offset_seconds};
  for (const auto *rule : {&tz.dst_start, &tz.dst_end}) {
    const auto rule_out = rule_fields(*rule);
    out.insert(out.end(), rule_out.begin(), rule_out.end());
  }
  return out;
}

TEST(Parse, AgreesWithPythonOnEveryZoneInTzdata) {
  const std::vector<Case> cases = read_fixture();
  size_t zones = 0;
  for (const Case &c : cases) {
    if (c.kind != "zone")
      continue;
    zones++;
    time::ParsedTimezone tz{};
    ASSERT_TRUE(c.ok) << c.label << " " << c.posix;
    ASSERT_TRUE(parse_posix_tz(c.posix, tz)) << c.label << " " << c.posix;
    EXPECT_EQ(fields_of(tz), c.fields) << c.label << " " << c.posix;
  }
  // The file was found and is whole: tzdata 2026.4 lists 598.
  EXPECT_GT(zones, 590u);
}

TEST(Parse, AgreesWithPythonOnTheOddAndTheMalformed) {
  size_t odd = 0;
  size_t malformed = 0;
  for (const Case &c : read_fixture()) {
    if (c.kind == "zone" || c.kind == "stricter")
      continue;
    (c.kind == "odd" ? odd : malformed)++;
    time::ParsedTimezone tz{};
    const bool ok = parse_posix_tz(c.posix, tz);
    EXPECT_EQ(ok, c.ok) << "'" << c.posix << "'";
    if (ok && c.ok)
      EXPECT_EQ(fields_of(tz), c.fields) << c.posix;
  }
  EXPECT_GT(odd, 30u);
  EXPECT_GT(malformed, 40u);
}

TEST(Parse, LeavesTheOutputAloneOnARefusal) {
  time::ParsedTimezone tz = parsed(NEW_YORK);
  const time::ParsedTimezone before = tz;
  EXPECT_FALSE(parse_posix_tz("EST5EDT,M3.2.0", tz));
  EXPECT_TRUE(same_zone(tz, before));
}

// Python's isalpha() and isdigit() take other scripts' letters and digits, and its parser
// skips trailing text of any kind; the device refuses every byte outside printable ASCII.
TEST(Parse, RefusesWhatIsNotPrintableAscii) {
  time::ParsedTimezone tz{};
  for (const std::string &text : {
           std::string("EST٣"),             // an Arabic-Indic three: Python reads 3 hours
           std::string("ESTé5"),            // a fourth letter to Python
           std::string("EST5é"),            // trailing to the device, a name to Python
           std::string("EST5EDTé,M3.2.0"),  // the same, after the DST name
           std::string("EST5\t"),           // trailing text Python ignores
           std::string("EST5\x7f"),         // DEL
           std::string("EST5\0EDT", 8),     // NUL: a stored copy would end there
       }) {
    EXPECT_FALSE(parse_posix_tz(text, tz)) << text;
  }
}

// generate_fixture.py's STRICTER: Python's int has no ceiling, and its parser no range.
TEST(Parse, RefusesWhatPythonTakesPastTheDevicesRange) {
  size_t stricter = 0;
  for (const Case &c : read_fixture()) {
    if (c.kind != "stricter")
      continue;
    stricter++;
    time::ParsedTimezone tz{};
    EXPECT_TRUE(c.ok) << c.posix;
    EXPECT_FALSE(parse_posix_tz(c.posix, tz)) << c.posix;
  }
  EXPECT_GT(stricter, 10u);
}

// Upstream negates an offset in 32 bits: EST-596523:14:08, INT32_MIN, made
// RealTimeClock::dump_config() overflow on the next boot.
TEST(Parse, KeepsOffsetsWithin24HoursOfUtc) {
  time::ParsedTimezone tz{};
  EXPECT_FALSE(parse_posix_tz("EST-596523:14:08", tz));
  for (const char *posix : {"EST24:00:01", "EST-24:00:01", "EST5EDT24:00:01,M3.2.0,M11.1.0",
                            "EST5EDT-24:00:01,M3.2.0,M11.1.0", "EST-23:00:01EDT,M3.2.0,M11.1.0"})
    EXPECT_FALSE(parse_posix_tz(posix, tz)) << posix;

  ASSERT_TRUE(parse_posix_tz("EST24:00:00", tz));
  EXPECT_EQ(tz.std_offset_seconds, 86400);
  ASSERT_TRUE(parse_posix_tz("EST-24:00:00", tz));
  EXPECT_EQ(tz.std_offset_seconds, -86400);
  ASSERT_TRUE(parse_posix_tz("EST5EDT24:00:00,M3.2.0,M11.1.0", tz));
  EXPECT_EQ(tz.dst_offset_seconds, 86400);
  ASSERT_TRUE(parse_posix_tz("EST5EDT-24:00:00,M3.2.0,M11.1.0", tz));
  EXPECT_EQ(tz.dst_offset_seconds, -86400);
  // The default DST offset, an hour ahead of standard time.
  ASSERT_TRUE(parse_posix_tz("EST-23EDT,M3.2.0,M11.1.0", tz));
  EXPECT_EQ(tz.dst_offset_seconds, -86400);
}

// A rule's time is only added, in 64 bits: anything the struct holds, either way.
TEST(Parse, TakesARuleTimeThe32BitsHold) {
  time::ParsedTimezone tz{};
  ASSERT_TRUE(parse_posix_tz("EST5EDT,M3.2.0/596523:14:07,M11.1.0/-596523:14:07", tz));
  EXPECT_EQ(tz.dst_start.time_seconds, INT32_MAX);
  EXPECT_EQ(tz.dst_end.time_seconds, -INT32_MAX);
  EXPECT_FALSE(parse_posix_tz("EST5EDT,M3.2.0/596523:14:08,M11.1.0", tz));
  EXPECT_FALSE(parse_posix_tz("EST5EDT,M3.2.0,M11.1.0/-596523:14:08", tz));
}

TEST(Parse, TakesNothingEmpty) {
  time::ParsedTimezone tz{};
  EXPECT_FALSE(parse_posix_tz("", tz));
}

}  // namespace esphome::timezone_settings::testing
