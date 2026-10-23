#include <cstring>
#include <functional>
#include "common.h"

namespace esphome::timezone_settings::testing {

TEST_F(Timezone, AFreshDeviceFollowsHomeAssistantOnTheCompiledZone) {
  TestSettings &settings = this->boot();
  EXPECT_TRUE(settings.follow_homeassistant());
  EXPECT_EQ(settings.zone_source(), ZoneSource::DEFAULT);
  EXPECT_FALSE(settings.has_chosen_zone());
  EXPECT_STREQ(settings.default_zone(), "Europe/Berlin");
  EXPECT_TRUE(this->in_effect(parsed(BERLIN)));
  EXPECT_EQ(settings.stores, 0);
}

// Released firmware stored Home Assistant's zone in the global and nothing else: such a device
// comes up on that zone, as it did before.
TEST_F(Timezone, ADeviceFromBeforeKeepsHomeAssistantsStoredZone) {
  this->homeassistant_zone = parsed(NEW_YORK);
  TestSettings &settings = this->boot();
  EXPECT_TRUE(settings.follow_homeassistant());
  EXPECT_EQ(settings.zone_source(), ZoneSource::HOMEASSISTANT);
  EXPECT_TRUE(this->in_effect(parsed(NEW_YORK)));
}

// The global cannot tell a stored UTC from nothing stored; it never could.
TEST_F(Timezone, AStoredUtcReadsAsNothingStored) {
  this->homeassistant_zone = parsed("UTC0");
  TestSettings &settings = this->boot();
  EXPECT_EQ(settings.zone_source(), ZoneSource::DEFAULT);
  EXPECT_TRUE(this->in_effect(parsed(BERLIN)));
}

TEST_F(Timezone, AnOffsetAloneIsAStoredZone) {
  this->homeassistant_zone = parsed(TOKYO);
  EXPECT_EQ(this->boot().zone_source(), ZoneSource::HOMEASSISTANT);
}

TEST_F(Timezone, AChosenZoneAppliesAtOnceWhenNotFollowing) {
  this->homeassistant_zone = parsed(NEW_YORK);
  TestSettings &settings = this->boot();
  ASSERT_EQ(stop_following(settings, Zone{"Asia/Tokyo", TOKYO}), SetError::NONE);
  EXPECT_FALSE(settings.follow_homeassistant());
  EXPECT_EQ(settings.zone_source(), ZoneSource::MANUAL);
  EXPECT_EQ(settings.chosen_zone().name, "Asia/Tokyo");
  EXPECT_EQ(settings.chosen_zone().posix, TOKYO);
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));
  // Home Assistant's is still there for when the device follows it again.
  EXPECT_TRUE(same_zone(this->homeassistant_zone, parsed(NEW_YORK)));
}

TEST_F(Timezone, TheChosenZoneAndTheModeSurviveAReboot) {
  stop_following(this->boot(), Zone{"Asia/Tokyo", TOKYO});
  time::set_global_tz(time::ParsedTimezone{});
  TestSettings &again = this->boot();
  EXPECT_FALSE(again.follow_homeassistant());
  EXPECT_EQ(again.zone_source(), ZoneSource::MANUAL);
  EXPECT_EQ(again.chosen_zone().name, "Asia/Tokyo");
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));
}

TEST_F(Timezone, AZoneChosenWhileFollowingIsKeptNotApplied) {
  TestSettings &settings = this->boot();
  ASSERT_EQ(settings.set_zone(Zone{"Asia/Tokyo", TOKYO}), SetError::NONE);
  EXPECT_TRUE(settings.follow_homeassistant());
  EXPECT_EQ(settings.zone_source(), ZoneSource::DEFAULT);
  EXPECT_TRUE(this->in_effect(parsed(BERLIN)));
  EXPECT_EQ(settings.chosen_zone().name, "Asia/Tokyo");

  TestSettings &again = this->boot();
  EXPECT_TRUE(again.follow_homeassistant());
  EXPECT_EQ(again.chosen_zone().posix, TOKYO);
  // And it is what not following then means.
  ASSERT_EQ(again.set_follow_homeassistant(false), SetError::NONE);
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));
}

TEST_F(Timezone, AZoneChosenWhileNotFollowingReplacesTheOneInEffect) {
  TestSettings &settings = this->boot();
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  ASSERT_EQ(settings.set_zone(Zone{"Asia/Kolkata", KOLKATA}), SetError::NONE);
  EXPECT_FALSE(settings.follow_homeassistant());
  EXPECT_TRUE(this->in_effect(parsed(KOLKATA)));
}

TEST_F(Timezone, FollowingAgainKeepsTheChosenZone) {
  this->homeassistant_zone = parsed(NEW_YORK);
  TestSettings &settings = this->boot();
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  ASSERT_EQ(settings.set_follow_homeassistant(true), SetError::NONE);
  EXPECT_EQ(settings.zone_source(), ZoneSource::HOMEASSISTANT);
  EXPECT_TRUE(this->in_effect(parsed(NEW_YORK)));
  EXPECT_EQ(settings.chosen_zone().name, "Asia/Tokyo");
}

TEST_F(Timezone, NotFollowingNeedsAZone) {
  TestSettings &settings = this->boot();
  EXPECT_EQ(settings.set_follow_homeassistant(false), SetError::NO_ZONE);
  EXPECT_TRUE(settings.follow_homeassistant());
  EXPECT_EQ(settings.stores, 0);
  EXPECT_TRUE(this->in_effect(parsed(BERLIN)));
}

TEST_F(Timezone, AZoneMayHaveNoName) {
  TestSettings &settings = this->boot();
  ASSERT_EQ(stop_following(settings, Zone{"", "<+0330>-3:30"}), SetError::NONE);
  EXPECT_EQ(settings.chosen_zone().name, "");
  EXPECT_TRUE(this->in_effect(parsed("<+0330>-3:30")));
  EXPECT_EQ(this->boot().chosen_zone().posix, "<+0330>-3:30");
}

TEST_F(Timezone, RefusesWhatItCouldNotStoreOrRun) {
  TestSettings &settings = this->boot();
  const std::string longest(63, 'A');
  EXPECT_EQ(settings.set_zone(Zone{std::string(64, 'a'), TOKYO}), SetError::NAME_TOO_LONG);
  EXPECT_EQ(settings.set_zone(Zone{"Asia/Tokyo", "JST-9" + std::string(59, ' ')}), SetError::POSIX_TOO_LONG);
  EXPECT_EQ(settings.set_zone(Zone{"Asia/Tokyo", "Asia/Tokyo"}), SetError::BAD_POSIX);
  EXPECT_EQ(settings.set_zone(Zone{"Asia/Tokyo", ""}), SetError::BAD_POSIX);
  EXPECT_EQ(settings.set_zone(Zone{"Asia/\nTokyo", TOKYO}), SetError::BAD_NAME);
  EXPECT_EQ(settings.set_zone(Zone{std::string("Asia\0Tokyo", 10), TOKYO}), SetError::BAD_NAME);
  EXPECT_EQ(settings.set_zone(Zone{"Asia/\x7fTokyo", TOKYO}), SetError::BAD_NAME);
  EXPECT_FALSE(settings.has_chosen_zone());
  EXPECT_EQ(settings.stores, 0);

  // Exactly at the limits, and a name in another script, are fine.
  ASSERT_EQ(settings.set_zone(Zone{longest, "AAA5" + std::string(59, ' ')}), SetError::NONE);
  EXPECT_EQ(settings.set_zone(Zone{"Европа/Москва", "MSK-3"}), SetError::NONE);
  EXPECT_EQ(this->boot().chosen_zone().name, "Европа/Москва");
}

TEST_F(Timezone, TheLongestZoneSurvivesAReboot) {
  const std::string name(63, 'n');
  const std::string posix = "AAA5" + std::string(59, ' ');
  ASSERT_EQ(stop_following(this->boot(), Zone{name, posix}), SetError::NONE);
  TestSettings &again = this->boot();
  EXPECT_EQ(again.chosen_zone().name, name);
  EXPECT_EQ(again.chosen_zone().posix, posix);
  EXPECT_EQ(again.zone_source(), ZoneSource::MANUAL);
}

TEST_F(Timezone, AStoreThatFailsChangesNothing) {
  TestSettings &settings = this->boot();
  settings.flush = TestSettings::Flush::FAILED_RECORD_LOST;
  EXPECT_EQ(stop_following(settings, Zone{"Asia/Tokyo", TOKYO}), SetError::NOT_STORED);
  EXPECT_TRUE(settings.follow_homeassistant());
  EXPECT_FALSE(settings.has_chosen_zone());
  EXPECT_TRUE(this->in_effect(parsed(BERLIN)));
  EXPECT_TRUE(LogCapture::instance().has("Storing the time settings failed"));
  EXPECT_FALSE(this->boot().has_chosen_zone());
}

// The flush reports for every record at once: a record that reads back as written is in flash.
TEST_F(Timezone, AFlushThatFailsForAnotherRecordStillApplies) {
  TestSettings &settings = this->boot();
  settings.flush = TestSettings::Flush::FAILED_RECORD_LANDED;
  EXPECT_EQ(stop_following(settings, Zone{"Asia/Tokyo", TOKYO}), SetError::NONE);
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));
  EXPECT_TRUE(LogCapture::instance().has("stored all the same"));
  EXPECT_EQ(this->boot().zone_source(), ZoneSource::MANUAL);
}

// A failed flush of a record that differs from the one in flash, not merely of the first one.
TEST_F(Timezone, AStoreThatFailsKeepsTheZoneStoredBefore) {
  TestSettings &settings = this->boot();
  ASSERT_EQ(stop_following(settings, Zone{"Asia/Tokyo", TOKYO}), SetError::NONE);
  settings.flush = TestSettings::Flush::FAILED_RECORD_LOST;
  EXPECT_EQ(settings.set_zone(Zone{"Asia/Kolkata", KOLKATA}), SetError::NOT_STORED);
  EXPECT_EQ(settings.chosen_zone().name, "Asia/Tokyo");
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));
  EXPECT_EQ(this->boot().chosen_zone().name, "Asia/Tokyo");
}

TEST_F(Timezone, AnyFieldThatChangesIsWritten) {
  TestSettings &settings = this->boot();
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  settings.set_zone(Zone{"Asia/Tokyo", "<+09>-9"});  // the string alone
  settings.set_zone(Zone{"Tokyo", "<+09>-9"});       // the name alone
  settings.set_follow_homeassistant(true);           // the mode alone
  EXPECT_EQ(settings.stores, 4);
  EXPECT_EQ(this->boot().chosen_zone().name, "Tokyo");
}

TEST_F(Timezone, WhatChangesNothingIsNotWrittenAgain) {
  TestSettings &settings = this->boot();
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  settings.set_follow_homeassistant(false);
  EXPECT_EQ(settings.stores, 1);
  // Even with a flash that would refuse it.
  settings.flush = TestSettings::Flush::FAILED_RECORD_LOST;
  EXPECT_EQ(settings.set_follow_homeassistant(false), SetError::NONE);
  EXPECT_EQ(settings.set_zone(Zone{"Asia/Kolkata", TOKYO}), SetError::NOT_STORED);
  EXPECT_EQ(settings.stores, 2);
}

TEST_F(Timezone, ARecordUnderAnotherKeyIsNotThisComponents) {
  stop_following(this->boot(), Zone{"Asia/Tokyo", TOKYO});
  TestSettings &other = this->boot("Europe/Berlin", BERLIN, PREF_HASH + 1);
  EXPECT_TRUE(other.follow_homeassistant());
  EXPECT_FALSE(other.has_chosen_zone());
}

// configure() never writes these; a damaged record falls back to following Home Assistant.
TEST_F(Timezone, ARecordThatMakesNoSenseFallsBack) {
  auto pref = global_preferences->make_preference<StoredSettings>(PREF_HASH);
  StoredSettings stored{};
  stored.follow_homeassistant = 0;
  std::strcpy(stored.name, "Nowhere/Garbage");
  std::strcpy(stored.posix, "not a zone");
  ASSERT_TRUE(pref.save(&stored));
  TestSettings &unparsable = this->boot();
  EXPECT_TRUE(unparsable.follow_homeassistant());
  EXPECT_FALSE(unparsable.has_chosen_zone());
  EXPECT_TRUE(LogCapture::instance().has("The stored zone 'not a zone' does not parse"));
  EXPECT_TRUE(this->in_effect(parsed(BERLIN)));

  stored.posix[0] = '\0';
  ASSERT_TRUE(pref.save(&stored));
  TestSettings &empty = this->boot();
  EXPECT_TRUE(empty.follow_homeassistant());
  EXPECT_FALSE(empty.has_chosen_zone());
}

// Every byte of both fields read up to the end, terminated or not.
TEST_F(Timezone, AnUnterminatedRecordIsCutAtItsFields) {
  auto pref = global_preferences->make_preference<StoredSettings>(PREF_HASH);
  StoredSettings stored{};
  stored.follow_homeassistant = 0;
  std::memset(stored.name, 'n', sizeof(stored.name));
  std::memset(stored.posix, ' ', sizeof(stored.posix));
  std::memcpy(stored.posix, "JST-9", 5);
  ASSERT_TRUE(pref.save(&stored));
  TestSettings &settings = this->boot();
  EXPECT_EQ(settings.chosen_zone().name, std::string(ZONE_NAME_MAX, 'n'));
  EXPECT_EQ(settings.chosen_zone().posix.size(), POSIX_MAX);
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));
}

TEST_F(Timezone, ABrokenCompiledZoneRunsOnUtc) {
  TestSettings &settings = this->boot("Broken", "not a zone");
  EXPECT_TRUE(LogCapture::instance().has("The compiled zone 'not a zone' does not parse"));
  EXPECT_TRUE(this->in_effect(time::ParsedTimezone{}));
  EXPECT_EQ(settings.zone_source(), ZoneSource::DEFAULT);
}

TEST_F(Timezone, SetsUpRightBelowTheGlobals) {
  // The globals restore Home Assistant's zone at HARDWARE; this has to see it.
  EXPECT_LT(this->boot().get_setup_priority(), setup_priority::HARDWARE);
  EXPECT_GT(this->device().settings.get_setup_priority(), setup_priority::HARDWARE - 2.0f);
}

TEST_F(Timezone, DumpsWhatIsInEffect) {
  TestSettings &settings = this->boot();
  settings.dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Compiled zone: Europe/Berlin"));
  EXPECT_TRUE(LogCapture::instance().has("Chosen zone: none"));
  EXPECT_TRUE(LogCapture::instance().has("In effect: default"));

  LogCapture::instance().clear();
  stop_following(settings, Zone{"", TOKYO});
  settings.dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Chosen zone: JST-9"));
  EXPECT_TRUE(LogCapture::instance().has("In effect: manual"));

  LogCapture::instance().clear();
  settings.set_zone(Zone{"Asia/Tokyo", TOKYO});
  settings.dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Chosen zone: Asia/Tokyo"));
}

// Every field counts: a zone that differs in one is another zone.
TEST(SameZone, ComparesEveryField) {
  const time::ParsedTimezone base = parsed(SYDNEY);
  EXPECT_TRUE(same_zone(base, base));
  std::vector<std::function<void(time::ParsedTimezone &)>> changes{
      [](time::ParsedTimezone &z) { z.std_offset_seconds++; },
      [](time::ParsedTimezone &z) { z.dst_offset_seconds++; },
  };
  for (auto member : {&time::ParsedTimezone::dst_start, &time::ParsedTimezone::dst_end}) {
    changes.emplace_back([member](time::ParsedTimezone &z) { (z.*member).time_seconds++; });
    changes.emplace_back([member](time::ParsedTimezone &z) { (z.*member).day++; });
    changes.emplace_back([member](time::ParsedTimezone &z) { (z.*member).type = time::DSTRuleType::DAY_OF_YEAR; });
    changes.emplace_back([member](time::ParsedTimezone &z) { (z.*member).month++; });
    changes.emplace_back([member](time::ParsedTimezone &z) { (z.*member).week++; });
    changes.emplace_back([member](time::ParsedTimezone &z) { (z.*member).day_of_week++; });
  }
  for (size_t i = 0; i < changes.size(); i++) {
    time::ParsedTimezone changed = base;
    changes[i](changed);
    EXPECT_FALSE(same_zone(base, changed)) << "field " << i;
    EXPECT_FALSE(same_zone(changed, base)) << "field " << i;
  }
}

TEST(Names, AreWhatTheApiSays) {
  EXPECT_STREQ(clock_source_name(ClockSource::HOMEASSISTANT), "homeassistant");
  EXPECT_STREQ(clock_source_name(ClockSource::SNTP), "sntp");
  EXPECT_STREQ(clock_source_name(ClockSource::RTC), "rtc");
  EXPECT_STREQ(clock_source_name(ClockSource::DASHBOARD), "dashboard");
  EXPECT_EQ(clock_source_name(ClockSource::NONE), nullptr);
  EXPECT_STREQ(zone_source_name(ZoneSource::HOMEASSISTANT), "homeassistant");
  EXPECT_STREQ(zone_source_name(ZoneSource::DEFAULT), "default");
  EXPECT_STREQ(zone_source_name(ZoneSource::MANUAL), "manual");
  EXPECT_STREQ(set_error_text(SetError::NAME_TOO_LONG), "'zone' is over 63 bytes");
  EXPECT_STREQ(set_error_text(SetError::BAD_NAME), "'zone' cannot contain control characters");
  EXPECT_STREQ(set_error_text(SetError::POSIX_TOO_LONG), "'posix' is over 63 bytes");
  EXPECT_STREQ(set_error_text(SetError::BAD_POSIX), "'posix' is not a POSIX TZ string");
  EXPECT_STREQ(set_error_text(SetError::NO_ZONE), "No zone was chosen yet: send 'posix' with it");
  EXPECT_STREQ(set_error_text(SetError::NOT_STORED), "The time settings were not stored");
  EXPECT_STREQ(set_error_text(SetError::TIME_OUT_OF_RANGE),
               "'time' must be from 1546300800 (2019-01-01) to 4102444799 (2099-12-31)");
  EXPECT_STREQ(set_error_text(SetError::CLOCK_NOT_SET), "The clock could not be set");
  EXPECT_STREQ(set_error_text(SetError::NONE), "");
}

}  // namespace esphome::timezone_settings::testing
