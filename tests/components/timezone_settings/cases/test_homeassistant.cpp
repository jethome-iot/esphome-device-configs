#include "common.h"

namespace esphome::timezone_settings::testing {

TEST_F(Timezone, HomeAssistantsZoneIsKeptAndFollowed) {
  TestSettings &settings = this->boot();
  const time::ParsedTimezone new_york = parsed(NEW_YORK);
  this->homeassistant_answers(&new_york);
  this->loop();
  EXPECT_EQ(settings.zone_source(), ZoneSource::HOMEASSISTANT);
  EXPECT_TRUE(this->in_effect(new_york));
  // Into the global, which saves it.
  EXPECT_TRUE(same_zone(this->homeassistant_zone, new_york));
  EXPECT_EQ(settings.stores, 0);

  // And it is what the next boot comes up on.
  time::set_global_tz(time::ParsedTimezone{});
  EXPECT_EQ(this->boot().zone_source(), ZoneSource::HOMEASSISTANT);
  EXPECT_TRUE(this->in_effect(new_york));
}

// Upstream sets the zone after the callbacks have run, so for one loop pass Home Assistant's is
// in effect; the deferred read puts the chosen one back.
TEST_F(Timezone, NotFollowingPutsTheChosenZoneBack) {
  TestSettings &settings = this->boot();
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  const time::ParsedTimezone new_york = parsed(NEW_YORK);
  this->homeassistant_answers(&new_york);
  EXPECT_TRUE(this->in_effect(new_york));
  this->loop();
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));
  EXPECT_EQ(settings.zone_source(), ZoneSource::MANUAL);
  // Kept for when the device follows again, which then needs no answer from Home Assistant.
  EXPECT_TRUE(same_zone(this->homeassistant_zone, new_york));
  ASSERT_EQ(settings.set_follow_homeassistant(true), SetError::NONE);
  EXPECT_TRUE(this->in_effect(new_york));
  EXPECT_EQ(settings.zone_source(), ZoneSource::HOMEASSISTANT);
}

// Home Assistant before 2026.3 sends no zone, only the time.
TEST_F(Timezone, AnAnswerWithoutAZoneChangesNothing) {
  this->homeassistant_zone = parsed(NEW_YORK);
  TestSettings &settings = this->boot();
  this->homeassistant_answers(nullptr);
  this->loop();
  EXPECT_TRUE(this->in_effect(parsed(NEW_YORK)));
  EXPECT_TRUE(same_zone(this->homeassistant_zone, parsed(NEW_YORK)));
  EXPECT_EQ(settings.clock_source(), ClockSource::HOMEASSISTANT);

  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  this->homeassistant_answers(nullptr);
  this->loop();
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));
  EXPECT_TRUE(same_zone(this->homeassistant_zone, parsed(NEW_YORK)));
}

// Following, that cannot be told from an answer carrying the zone in effect: the compiled zone
// stays, taken for Home Assistant's.
TEST_F(Timezone, AnAnswerWithoutAZoneLeavesTheCompiledOneInEffect) {
  TestSettings &settings = this->boot();
  this->homeassistant_answers(nullptr);
  this->loop();
  EXPECT_TRUE(this->in_effect(parsed(BERLIN)));
  EXPECT_EQ(settings.zone_source(), ZoneSource::HOMEASSISTANT);
  EXPECT_TRUE(same_zone(this->homeassistant_zone, parsed(BERLIN)));
}

// A stored UTC reads as nothing stored at the next boot, but one sent now is Home Assistant's.
TEST_F(Timezone, HomeAssistantsUtcIsFollowedForTheBoot) {
  TestSettings &settings = this->boot();
  const time::ParsedTimezone utc{};
  this->homeassistant_answers(&utc);
  this->loop();
  EXPECT_EQ(settings.zone_source(), ZoneSource::HOMEASSISTANT);
  EXPECT_TRUE(this->in_effect(utc));

  EXPECT_EQ(this->boot().zone_source(), ZoneSource::DEFAULT);
  EXPECT_TRUE(this->in_effect(parsed(BERLIN)));
}

// Not following, the snapshot is the zone from before the first answer: a second one in the same
// pass, carrying the same zone, must not hide that the first changed it.
TEST_F(Timezone, TwoAnswersInOnePassKeepTheFirstSnapshot) {
  TestSettings &settings = this->boot();
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  const time::ParsedTimezone new_york = parsed(NEW_YORK);
  this->homeassistant_answers(&new_york);
  this->homeassistant_answers(&new_york);
  this->loop();
  EXPECT_TRUE(same_zone(this->homeassistant_zone, new_york));
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));

  // The next pass takes a snapshot of its own: Tokyo is no longer the zone in force.
  ASSERT_EQ(settings.set_zone(Zone{"Asia/Kolkata", KOLKATA}), SetError::NONE);
  const time::ParsedTimezone tokyo = parsed(TOKYO);
  this->homeassistant_answers(&tokyo);
  this->loop();
  EXPECT_TRUE(same_zone(this->homeassistant_zone, tokyo));
  EXPECT_TRUE(this->in_effect(parsed(KOLKATA)));
}

// Following, the zone in force after an answer is Home Assistant's, even when it is the compiled
// one it was already on.
TEST_F(Timezone, AnAnswerCarryingTheZoneInEffectIsHomeAssistantsWhenFollowing) {
  TestSettings &settings = this->boot();
  const time::ParsedTimezone berlin = parsed(BERLIN);
  this->homeassistant_answers(&berlin);
  this->loop();
  EXPECT_EQ(settings.zone_source(), ZoneSource::HOMEASSISTANT);
  EXPECT_TRUE(this->in_effect(berlin));
  EXPECT_TRUE(same_zone(this->homeassistant_zone, berlin));
  EXPECT_EQ(this->boot().zone_source(), ZoneSource::HOMEASSISTANT);
}

// Not following, the chosen zone is in force: an answer carrying it cannot be told from none, and
// Home Assistant's zone stays as it was.
TEST_F(Timezone, AnAnswerCarryingTheChosenZoneLooksLikeNoneWhenNotFollowing) {
  this->homeassistant_zone = parsed(NEW_YORK);
  TestSettings &settings = this->boot();
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  const time::ParsedTimezone tokyo = parsed(TOKYO);
  this->homeassistant_answers(&tokyo);
  this->loop();
  EXPECT_TRUE(same_zone(this->homeassistant_zone, parsed(NEW_YORK)));
  EXPECT_TRUE(this->in_effect(tokyo));
  EXPECT_EQ(settings.zone_source(), ZoneSource::MANUAL);
}

// So following again asks Home Assistant at once, rather than at its next poll: its zone may have
// changed meanwhile, to the chosen one as well.
TEST_F(Timezone, FollowingAgainAsksHomeAssistant) {
  this->homeassistant_zone = parsed(NEW_YORK);
  TestSettings &settings = this->boot();
  const FakeClock &homeassistant = this->device().homeassistant;
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  settings.flush = TestSettings::Flush::FAILED_RECORD_LOST;
  ASSERT_EQ(settings.set_follow_homeassistant(true), SetError::NOT_STORED);
  EXPECT_EQ(homeassistant.updates, 0);

  settings.flush = TestSettings::Flush::OK;
  ASSERT_EQ(settings.set_follow_homeassistant(true), SetError::NONE);
  EXPECT_EQ(homeassistant.updates, 1);
  const time::ParsedTimezone tokyo = parsed(TOKYO);
  this->homeassistant_answers(&tokyo);
  this->loop();
  EXPECT_TRUE(same_zone(this->homeassistant_zone, tokyo));
  EXPECT_TRUE(this->in_effect(tokyo));

  // Only following again asks.
  ASSERT_EQ(settings.set_follow_homeassistant(true), SetError::NONE);
  ASSERT_EQ(settings.set_zone(Zone{"Asia/Kolkata", KOLKATA}), SetError::NONE);
  ASSERT_EQ(settings.set_follow_homeassistant(false), SetError::NONE);
  EXPECT_EQ(homeassistant.updates, 1);
}

TEST_F(Timezone, FollowingAgainWithoutHomeAssistantsClockAsksNoOne) {
  TestSettings settings;
  settings.set_default_zone("Europe/Berlin", BERLIN);
  settings.set_preference_hash(PREF_HASH);
  settings.setup();
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  ASSERT_EQ(settings.set_follow_homeassistant(true), SetError::NONE);
  EXPECT_TRUE(this->in_effect(parsed(BERLIN)));
}

// A change that runs between an answer and the read of its zone -- one the loop task had queued
// ahead of the read -- must not be taken for the zone Home Assistant sent.
TEST_F(Timezone, AChangeBeforeTheReadIsNotTakenForHomeAssistantsZone) {
  TestSettings &settings = this->boot();
  const time::ParsedTimezone new_york = parsed(NEW_YORK);
  this->homeassistant_answers(&new_york);
  ASSERT_EQ(stop_following(settings, Zone{"Asia/Tokyo", TOKYO}), SetError::NONE);
  this->loop();
  EXPECT_TRUE(same_zone(this->homeassistant_zone, new_york));
  EXPECT_TRUE(this->in_effect(parsed(TOKYO)));
  EXPECT_EQ(settings.zone_source(), ZoneSource::MANUAL);
}

TEST_F(Timezone, FollowingAgainBeforeTheReadIsNotTakenForHomeAssistantsZone) {
  this->homeassistant_zone = parsed(SYDNEY);
  TestSettings &settings = this->boot();
  stop_following(settings, Zone{"Asia/Tokyo", TOKYO});
  const time::ParsedTimezone new_york = parsed(NEW_YORK);
  this->homeassistant_answers(&new_york);
  ASSERT_EQ(settings.set_follow_homeassistant(true), SetError::NONE);
  this->loop();
  EXPECT_TRUE(same_zone(this->homeassistant_zone, new_york));
  EXPECT_TRUE(this->in_effect(new_york));
  EXPECT_EQ(settings.zone_source(), ZoneSource::HOMEASSISTANT);
}

TEST_F(Timezone, TheNetworkClockThatSyncedLastIsReported) {
  TestSettings &settings = this->boot();
  EXPECT_EQ(settings.clock_source(), ClockSource::NONE);
  this->device().rtc.sync();
  EXPECT_EQ(settings.clock_source(), ClockSource::RTC);
  this->device().sntp.sync();
  EXPECT_EQ(settings.clock_source(), ClockSource::SNTP);
  this->device().homeassistant.sync();
  EXPECT_EQ(settings.clock_source(), ClockSource::HOMEASSISTANT);
  // The RTC's periodic re-read does not hide that the network set the clock.
  this->device().rtc.sync();
  EXPECT_EQ(settings.clock_source(), ClockSource::HOMEASSISTANT);
  this->device().sntp.sync();
  EXPECT_EQ(settings.clock_source(), ClockSource::SNTP);
  this->loop();
  // RAM only: a reboot has heard from none of them yet.
  EXPECT_EQ(this->boot().clock_source(), ClockSource::NONE);
}

// The other clocks set no zone: their syncs touch nothing but the report.
TEST_F(Timezone, OnlyHomeAssistantsClockHandsOverAZone) {
  TestSettings &settings = this->boot();
  this->device().sntp.sync();
  time::set_global_tz(parsed(NEW_YORK));
  this->loop();
  EXPECT_EQ(settings.zone_source(), ZoneSource::DEFAULT);
  EXPECT_FALSE(this->homeassistant_zone.has_dst());
}

}  // namespace esphome::timezone_settings::testing
