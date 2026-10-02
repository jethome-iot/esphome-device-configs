#include "common.h"

namespace esphome::mqtt_config::testing {

static const char *const CLEANING = "Saved; removing this device's Home Assistant entries";
static const char *const CLEANING_PENDING =
    "Saved; this device's Home Assistant entries go once the broker is reachable";
static const char *const CLEANING_THEN_REBOOT =
    "Saved; removing this device's Home Assistant entries now, the rest applies after a reboot";
static const char *const REBOOT_ENTRIES_STAY =
    "Saved; applies after a reboot. The broker is not reachable, so this device's Home Assistant entries stay on it "
    "unless it comes back before then";
static const char *const REBOOT = "Saved; applies after a reboot";

class CleanupTest : public MqttTest {
 protected:
  // A device running with discovery on, its entries announced.
  TestConfig &announced(bool connected = true) {
    MqttRecord stored = enabled_record();
    stored.discovery = true;
    this->plant(stored);
    TestConfig &c = this->boot();
    if (connected) {
      this->connect_and_settle();
      EXPECT_EQ(this->discovery_publishes("{}"), 3u);
      this->client->published.clear();
    }
    return c;
  }
  std::vector<std::string> cleaned_topics() const {
    std::vector<std::string> topics;
    for (const auto &p : this->client->published) {
      if (p.payload.empty() && p.retain)
        topics.push_back(p.topic);
    }
    return topics;
  }
};

TEST_F(CleanupTest, DiscoveryOffWhileConnectedRemovesTheEntriesWithoutAReboot) {
  TestConfig &c = this->announced();
  const auto result = this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  EXPECT_EQ(result.code, 200);
  EXPECT_STREQ(result.message, CLEANING);
  EXPECT_FALSE(result.reboot_required);
  EXPECT_EQ(result.cleanup, DiscoveryCleanup::RUNNING);
  EXPECT_TRUE(this->client->get_discovery_info().clean);
  EXPECT_TRUE(from_stored(this->board.record()).clean_pending);

  // Resends only for what the client serves: never the hidden one.
  EXPECT_TRUE(this->relay1->is_resend_pending());
  EXPECT_FALSE(this->hidden->is_resend_pending());
  this->settle();
  const std::string node = NODE;
  EXPECT_EQ(this->cleaned_topics(),
            (std::vector<std::string>{"homeassistant/switch/" + node + "/relay_1/config",
                                      "homeassistant/switch/" + node + "/relay_2/config",
                                      "homeassistant/binary_sensor/" + node + "/input_1/config"}));

  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
  EXPECT_FALSE(from_stored(this->board.record()).clean_pending);
  EXPECT_FALSE(c.reboot_required());
  EXPECT_FALSE(this->settings_json()["running"]["discovery"].as<bool>());
}

// Clean mode is set at save time; the next connect's resend is what finishes it.
TEST_F(CleanupTest, DiscoveryOffWhileDisconnectedFinishesOnTheNextConnect) {
  TestConfig &c = this->announced();
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  const auto result = this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  EXPECT_STREQ(result.message, CLEANING_PENDING);
  EXPECT_EQ(result.cleanup, DiscoveryCleanup::PENDING);
  EXPECT_FALSE(result.reboot_required);
  EXPECT_TRUE(this->client->get_discovery_info().clean);
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::PENDING);

  this->connect_and_settle();
  EXPECT_EQ(this->cleaned_topics().size(), 3u);
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
  EXPECT_FALSE(from_stored(this->board.record()).clean_pending);
}

TEST_F(CleanupTest, MqttOffWhileConnectedRemovesTheEntriesThenWaitsForTheReboot) {
  TestConfig &c = this->announced();
  const auto result = this->save(patch_of([](MqttPatch &p) { p.enabled = false; }));
  EXPECT_STREQ(result.message, CLEANING_THEN_REBOOT);
  EXPECT_TRUE(result.reboot_required);
  EXPECT_EQ(result.cleanup, DiscoveryCleanup::RUNNING);
  this->settle();
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
  EXPECT_TRUE(c.reboot_required());
  const MqttRecord stored = from_stored(this->board.record());
  EXPECT_FALSE(stored.enabled);
  EXPECT_FALSE(stored.clean_pending);
}

// Stock cannot remove them after the reboot: nothing connects then. The answer says so.
TEST_F(CleanupTest, MqttOffWhileDisconnectedLeavesTheEntriesAndSaysSo) {
  TestConfig &c = this->announced();
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  const auto result = this->save(patch_of([](MqttPatch &p) { p.enabled = false; }));
  EXPECT_STREQ(result.message, REBOOT_ENTRIES_STAY);
  EXPECT_TRUE(result.reboot_required);
  EXPECT_EQ(result.cleanup, DiscoveryCleanup::PENDING);

  // After the reboot nothing connects, and the flag waits for MQTT to come back on.
  TestConfig &next = this->reboot();
  EXPECT_FALSE(next.running());
  EXPECT_EQ(next.discovery_cleanup(), DiscoveryCleanup::PENDING);
  (void) c;
}

// A broker change cleans the broker that has them; the new one gets them announced again.
TEST_F(CleanupTest, ABrokerChangeCleansTheOldBrokerAndWaitsForTheReboot) {
  TestConfig &c = this->announced();
  const auto result = this->save(patch_of([](MqttPatch &p) { p.broker = std::string("192.168.1.20"); }));
  EXPECT_STREQ(result.message, CLEANING_THEN_REBOOT);
  EXPECT_TRUE(result.reboot_required);
  EXPECT_FALSE(from_stored(this->board.record()).clean_pending);
  this->settle();
  EXPECT_EQ(this->cleaned_topics().size(), 3u);
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
}

// Clean mode stays for the rest of the boot, so putting the broker back does not bring the
// entries back: that takes the reboot.
TEST_F(CleanupTest, ABrokerPutBackBeforeTheRebootStillNeedsIt) {
  TestConfig &c = this->announced();
  this->save(patch_of([](MqttPatch &p) { p.broker = std::string("192.168.1.20"); }));
  const auto back = this->save(patch_of([](MqttPatch &p) { p.broker = std::string("192.168.1.10"); }));
  EXPECT_STREQ(back.message, CLEANING_THEN_REBOOT);
  EXPECT_TRUE(back.reboot_required);
  EXPECT_TRUE(c.reboot_required());
  EXPECT_TRUE(this->client->get_discovery_info().clean);
  EXPECT_FALSE(this->settings_json()["running"]["discovery"].as<bool>());
}

TEST_F(CleanupTest, APortChangeCleansToo) {
  this->announced();
  const auto result = this->save(patch_of([](MqttPatch &p) { p.port = 1884; }));
  EXPECT_STREQ(result.message, CLEANING_THEN_REBOOT);
}

// The config topic does not depend on them.
TEST_F(CleanupTest, APrefixClientIdOrCredentialChangeLeavesTheEntries) {
  TestConfig &c = this->announced();
  const auto result = this->save(patch_of([](MqttPatch &p) {
    p.topic_prefix = std::string("home/kitchen");
    p.client_id = std::string("jxd-kitchen");
    p.username = std::string("jxd");
    p.password = std::string("pw");
  }));
  EXPECT_STREQ(result.message, REBOOT);
  EXPECT_EQ(result.cleanup, DiscoveryCleanup::NONE);
  EXPECT_FALSE(this->client->get_discovery_info().clean);
  EXPECT_FALSE(this->relay1->is_resend_pending());
  (void) c;
}

// On applies after a reboot, off applies now: turning it back on keeps the cleanup going.
TEST_F(CleanupTest, DiscoveryBackOnWaitsForTheReboot) {
  TestConfig &c = this->announced();
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  const auto result = this->save(patch_of([](MqttPatch &p) { p.discovery = true; }));
  EXPECT_STREQ(result.message,
               "Saved; removing this device's Home Assistant entries now, the rest applies after a reboot");
  EXPECT_TRUE(c.reboot_required());
  EXPECT_FALSE(from_stored(this->board.record()).clean_pending);
}

// is_connected() turns true in the same client pass that schedules the resends, so the loop
// never sees "connected, nothing pending" before they exist. This pins the stand-in's copy of
// that order (upstream's check_connected()), not upstream itself.
TEST_F(CleanupTest, CompletionIsNeverSeenBetweenTheConnectAndTheResends) {
  MqttRecord stored = enabled_record();
  stored.clean_pending = true;
  this->plant(stored);
  TestConfig &c = this->boot();
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::PENDING);
  this->client->connect_for_test();
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::RUNNING);
  // One pass handles at most eight; these are three, so one pass empties it.
  EXPECT_EQ(this->client->process_resends_for_test(), 3u);
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
  EXPECT_FALSE(from_stored(this->board.record()).clean_pending);
}

TEST_F(CleanupTest, AFailedPublishKeepsTheCleanupRunning) {
  TestConfig &c = this->announced();
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  this->client->fail_publishes = true;
  this->client->process_resends_for_test();
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::RUNNING);
  this->client->fail_publishes = false;
  this->settle();
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
}

// The cleanup finished, but the flag could not be cleared: the next boot runs the fallback.
TEST_F(CleanupTest, AFailedFlagWriteLeavesTheFallbackForTheNextBoot) {
  TestConfig &c = this->announced();
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  c.flush = TestConfig::Flush::FAILED_RECORD_LOST;
  this->settle();
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::PENDING);  // still flagged in NVS and RAM
  TestConfig &next = this->reboot();
  EXPECT_TRUE(this->client->get_discovery_info().clean);
  EXPECT_EQ(next.discovery_cleanup(), DiscoveryCleanup::PENDING);
}

// The flush reports for every key at once; the cleared flag reached NVS all the same.
TEST_F(CleanupTest, AFlagWriteWhoseFlushFailedForAnotherRecordStillClearsIt) {
  TestConfig &c = this->announced();
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  c.flush = TestConfig::Flush::FAILED_RECORD_LANDED;
  this->settle();
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
  EXPECT_FALSE(from_stored(this->board.record()).clean_pending);
}

TEST_F(CleanupTest, TheLoopStopsItselfWhenNothingRuns) {
  TestConfig &c = this->announced();
  c.run_loop();
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::NONE);
  EXPECT_FALSE(this->client->get_discovery_info().clean);
}

TEST_F(CleanupTest, AfterCleanupRunsAtOnceWhenNoneRuns) {
  this->announced();
  int ran = 0;
  this->config->after_cleanup(5000, [&ran]() { ran++; });
  EXPECT_EQ(ran, 1);
}

// Pending is not running: with the broker away there is nothing to wait for.
TEST_F(CleanupTest, AfterCleanupDoesNotWaitForAPendingOne) {
  this->announced();
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  int ran = 0;
  this->config->after_cleanup(5000, [&ran]() { ran++; });
  EXPECT_EQ(ran, 1);
}

TEST_F(CleanupTest, AfterCleanupWaitsForTheCleanupToFinish) {
  this->announced();
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  int ran = 0;
  this->config->after_cleanup(5000, [&ran]() { ran++; });
  EXPECT_EQ(ran, 0);
  advance(150);
  EXPECT_EQ(ran, 0);
  this->settle();
  advance(150);
  EXPECT_EQ(ran, 1);
  advance(150);
  EXPECT_EQ(ran, 1);
}

TEST_F(CleanupTest, AfterCleanupGivesUpAfterItsTimeout) {
  TestConfig &c = this->announced();
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  int first = 0;
  int second = 0;
  c.after_cleanup(50, [&first]() { first++; });
  c.after_cleanup(5000, [&second]() { second++; });
  advance(150);
  EXPECT_EQ(first, 1);
  EXPECT_EQ(second, 0);
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::RUNNING);
  this->settle();
  advance(150);
  EXPECT_EQ(second, 1);
}

// A dropped connection ends the wait: nothing more goes out until it is back.
TEST_F(CleanupTest, AfterCleanupStopsWaitingWhenTheConnectionDrops) {
  this->announced();
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  int ran = 0;
  this->config->after_cleanup(5000, [&ran]() { ran++; });
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  advance(150);
  EXPECT_EQ(ran, 1);
}

// The record goes with NVS, so the entries leave the broker first.
TEST_F(CleanupTest, AFactoryResetRemovesTheEntriesFirst) {
  TestConfig &c = this->announced();
  int wiped = 0;
  c.before_factory_reset([&wiped]() { wiped++; });
  EXPECT_EQ(wiped, 0);
  EXPECT_TRUE(this->client->get_discovery_info().clean);
  EXPECT_EQ(c.discovery_cleanup(), DiscoveryCleanup::RUNNING);
  this->settle();
  EXPECT_EQ(this->cleaned_topics().size(), 3u);
  advance(150);
  EXPECT_EQ(wiped, 1);
}

TEST_F(CleanupTest, AFactoryResetWithoutDiscoveryWipesAtOnce) {
  MqttRecord stored = enabled_record();
  this->plant(stored);
  TestConfig &c = this->boot();
  this->connect_and_settle();
  int wiped = 0;
  c.before_factory_reset([&wiped]() { wiped++; });
  EXPECT_EQ(wiped, 1);
  EXPECT_FALSE(this->client->is_discovery_enabled());
}

// Disconnected, nothing can go out before the wipe; the entries stay (doc/MQTT.md says so).
TEST_F(CleanupTest, AFactoryResetWhileDisconnectedWipesAtOnce) {
  TestConfig &c = this->announced();
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  int wiped = 0;
  c.before_factory_reset([&wiped]() { wiped++; });
  EXPECT_EQ(wiped, 1);
}

TEST_F(CleanupTest, AFactoryResetWithMqttOffWipesAtOnce) {
  TestConfig &c = this->boot();
  int wiped = 0;
  c.before_factory_reset([&wiped]() { wiped++; });
  EXPECT_EQ(wiped, 1);
}

}  // namespace esphome::mqtt_config::testing
