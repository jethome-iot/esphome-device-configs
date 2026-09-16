#include "common.h"

namespace esphome::automations::testing {

static bool load_cron(const std::string &cron, TriggerConfig &out) {
  const std::string json = R"({"source":"cron","cron":")" + cron + R"("})";
  return load(json.c_str(), out);
}

static std::string round_trip(const char *cron) {
  TriggerConfig t;
  if (!load_cron(cron, t))
    return "<refused>";
  return t.cron_string();
}

TEST(CronField, RoundTripsEveryForm) {
  EXPECT_EQ(round_trip("* * * * * *"), "* * * * * *");
  EXPECT_EQ(round_trip("0 */5 8-17 * * 1-5"), "0 */5 8-17 * * 1-5");
  EXPECT_EQ(round_trip("0,30 * * * * *"), "0,30 * * * * *");
  EXPECT_EQ(round_trip("0 0 12 1,15 * *"), "0 0 12 1,15 * *");
  EXPECT_EQ(round_trip("0 0 0 * * 7"), "0 0 0 * * 7");
  EXPECT_EQ(round_trip("0 0 6 * 1-3/1 *"), "0 0 6 * 1-3 *");
}

TEST(CronField, ClampsARangeToTheField) {
  EXPECT_EQ(round_trip("0 0 0 1-40 * *"), "0 0 0 * * *");
  EXPECT_EQ(round_trip("0 0 20-30 * * *"), "0 0 20-23 * * *");
}

TEST(CronField, RefusesAFieldThatNeverMatches) {
  EXPECT_EQ(round_trip("99 * * * * *"), "<refused>");
  EXPECT_EQ(round_trip("5-3 * * * * *"), "<refused>");
  EXPECT_EQ(round_trip("*/0 * * * * *"), "<refused>");
  EXPECT_EQ(round_trip("a * * * * *"), "<refused>");
  EXPECT_EQ(round_trip("* * * * * 0"), "<refused>");
}

TEST(CronField, RefusesTheWrongNumberOfFields) {
  EXPECT_EQ(round_trip("0 0 0 * *"), "<refused>");
  EXPECT_EQ(round_trip("0 0 0 * * * *"), "<refused>");
  TriggerConfig t;
  EXPECT_FALSE(load(R"({"source":"cron"})", t));
}

TEST(CronPreset, IsKeptOnlyWhenTheFileHasOne) {
  TriggerConfig with;
  ASSERT_TRUE(load(R"({"source":"cron","cron":"0 0 6 * * *","cron_preset":"daily"})", with));
  ASSERT_TRUE(with.cron_preset.has_value());
  EXPECT_EQ(*with.cron_preset, CronPreset::DAILY);
  EXPECT_EQ(dump(with), R"({"source":"cron","cron":"0 0 6 * * *","cron_preset":"daily"})");

  TriggerConfig without;
  ASSERT_TRUE(load(R"({"source":"cron","cron":"0 0 6 * * *"})", without));
  EXPECT_FALSE(without.cron_preset.has_value());
  EXPECT_EQ(dump(without), R"({"source":"cron","cron":"0 0 6 * * *"})");

  TriggerConfig misspelt;
  EXPECT_FALSE(load(R"({"source":"cron","cron":"0 0 6 * * *","cron_preset":"dayly"})", misspelt));
}

// A valid ESPTime: the calendar fields have to agree with each other.
static ESPTime at(uint8_t hour, uint8_t minute, uint8_t second, uint8_t day_of_week = 2) {
  ESPTime t{};
  t.second = second;
  t.minute = minute;
  t.hour = hour;
  t.day_of_week = day_of_week;
  t.day_of_month = 2;
  t.day_of_year = 33;
  t.month = 2;
  t.year = 2026;
  return t;
}

TEST(CronTrigger, NeedsAClock) {
  FakeEngine engine;
  TriggerConfig config;
  ASSERT_TRUE(load_cron("* * * * * *", config));
  CompiledTrigger trigger;
  EXPECT_FALSE(compile_trigger(&engine, config, trigger));
  engine.with_clock();
  EXPECT_TRUE(compile_trigger(&engine, config, trigger));
}

TEST(CronTrigger, MatchesTheListedMomentOnly) {
  FakeEngine engine;
  engine.with_clock();
  TriggerConfig config;
  ASSERT_TRUE(load_cron("30 15 10 * * 1-5", config));
  CompiledTrigger trigger;
  ASSERT_TRUE(compile_trigger(&engine, config, trigger));
  ASSERT_TRUE(at(10, 15, 30).is_valid());
  EXPECT_TRUE(trigger.cron_matches(at(10, 15, 30)));
  EXPECT_FALSE(trigger.cron_matches(at(10, 15, 31)));
  EXPECT_FALSE(trigger.cron_matches(at(10, 16, 30)));
  EXPECT_FALSE(trigger.cron_matches(at(11, 15, 30)));
  EXPECT_FALSE(trigger.cron_matches(at(10, 15, 30, 7)));
}

TEST(CronTrigger, IgnoresAnInvalidTime) {
  FakeEngine engine;
  engine.with_clock();
  TriggerConfig config;
  ASSERT_TRUE(load_cron("* * * * * *", config));
  CompiledTrigger trigger;
  ASSERT_TRUE(compile_trigger(&engine, config, trigger));
  ESPTime unset{};
  EXPECT_FALSE(trigger.cron_matches(unset));
}

}  // namespace esphome::automations::testing
