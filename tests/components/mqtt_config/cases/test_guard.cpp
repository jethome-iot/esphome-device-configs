#include "common.h"

namespace esphome::mqtt_config::testing {

static CrashGuardRecord guard(uint8_t streak, bool armed) {
  return CrashGuardRecord{CRASH_GUARD_MAGIC, streak, static_cast<uint8_t>(armed ? 1 : 0), {0, 0}};
}

TEST(MqttCrashGuard, NextStreakTruthTable) {
  // Up by one only after a panic or watchdog reset that came while armed.
  EXPECT_EQ(next_streak(guard(0, true), true, true), 1);
  EXPECT_EQ(next_streak(guard(2, true), true, true), 3);
  EXPECT_EQ(next_streak(guard(2, false), true, true), 0);
  EXPECT_EQ(next_streak(guard(2, true), true, false), 0);
  EXPECT_EQ(next_streak(guard(2, false), true, false), 0);
  // Power-on garbage counts as nothing, whatever it holds.
  EXPECT_EQ(next_streak(guard(2, true), false, true), 0);
  EXPECT_EQ(next_streak(guard(255, true), true, true), 255);
}

TEST(MqttCrashGuard, TheStagesAreTwoAndThree) {
  EXPECT_EQ(SUSPEND_SUBSCRIPTIONS_AT, 2);
  EXPECT_EQ(HOLD_MQTT_AT, 3);
  EXPECT_LT(SUSPEND_SUBSCRIPTIONS_AT, HOLD_MQTT_AT);
  EXPECT_EQ(DISARM_AFTER_MS, 60000u);
}

class GuardTest : public MqttTest {
 protected:
  TestConfig &boot_after(uint8_t streak, bool armed, bool panic) {
    this->plant(enabled_record());
    this->board.rtc = guard(streak, armed);
    this->board.panic = panic;
    return this->boot([](TestConfig &c) { c.set_disarm_after_ms(20); });
  }
};

TEST_F(GuardTest, TheSlotsStageStillRunsMqtt) {
  TestConfig &c = this->boot_after(1, true, true);
  EXPECT_EQ(c.crash_streak(), 2);
  EXPECT_TRUE(c.running());
  EXPECT_FALSE(c.reboot_required());
}

TEST_F(GuardTest, TheThirdCrashHoldsMqttBack) {
  TestConfig &c = this->boot_after(2, true, true);
  EXPECT_EQ(c.crash_streak(), 3);
  EXPECT_FALSE(c.running());
  EXPECT_EQ(c.last_error(), MqttError::CRASH_GUARD);
  EXPECT_TRUE(c.reboot_required());
}

// Any software reset or power cycle resets it, so the reboot the dashboard offers retries.
TEST_F(GuardTest, ASoftResetClearsTheStreak) {
  TestConfig &c = this->boot_after(5, true, false);
  EXPECT_EQ(c.crash_streak(), 0);
  EXPECT_TRUE(c.running());
}

TEST_F(GuardTest, ABadMagicIsNoStreak) {
  this->plant(enabled_record());
  this->board.rtc = CrashGuardRecord{0xDEADBEEF, 7, 1, {0, 0}};
  this->board.panic = true;
  TestConfig &c = this->boot();
  EXPECT_EQ(c.crash_streak(), 0);
  EXPECT_EQ(this->board.rtc.magic, CRASH_GUARD_MAGIC);
}

// Evaluated once, by whoever asks first: mqtt_subscriptions asks before this setup runs.
TEST_F(GuardTest, TheStreakIsEvaluatedOncePerBoot) {
  TestConfig &c = this->boot_after(1, true, true);
  EXPECT_EQ(c.crash_streak(), 2);
  EXPECT_EQ(this->board.rtc.streak, 2);
  EXPECT_EQ(this->board.rtc.armed, 0);
  this->board.rtc.armed = 1;
  EXPECT_EQ(c.crash_streak(), 2);
}

TEST_F(GuardTest, AConnectArmsItAndALastingConnectionDisarmsIt) {
  TestConfig &c = this->boot_after(1, true, true);
  ASSERT_EQ(c.crash_streak(), 2);
  EXPECT_EQ(this->board.rtc.armed, 0);
  this->client->connect_for_test();
  EXPECT_EQ(this->board.rtc.armed, 1);
  advance(40);
  EXPECT_EQ(this->board.rtc.armed, 0);
  EXPECT_EQ(this->board.rtc.streak, 0);
}

TEST_F(GuardTest, ADisconnectBeforeThenKeepsItArmed) {
  TestConfig &c = this->boot_after(1, true, true);
  this->client->connect_for_test();
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  advance(40);
  EXPECT_EQ(this->board.rtc.armed, 1);
  EXPECT_EQ(this->board.rtc.streak, 2);
  (void) c;
}

// A crash while armed counts, one while disarmed does not; the next boot reads which.
TEST_F(GuardTest, TheStreakCarriesAcrossBoots) {
  this->boot_after(0, false, true);
  this->client->connect_for_test();  // armed, then the device panics
  TestConfig &second = this->reboot();
  EXPECT_EQ(second.crash_streak(), 1);
  this->client->connect_for_test();
  TestConfig &third = this->reboot();
  EXPECT_EQ(third.crash_streak(), 2);
  this->board.panic = false;
  TestConfig &fourth = this->reboot();
  EXPECT_EQ(fourth.crash_streak(), 0);
}

}  // namespace esphome::mqtt_config::testing
