#include "common.h"

namespace esphome::mqtt_subscriptions::testing {

// A relay's command topic, which the client subscribes itself on every connect.
static const char *const COMMAND = "mqtt-subscriptions-test/switch/relay_1/command";

static mqtt_config::CrashGuardRecord armed_streak(uint8_t streak) {
  return mqtt_config::CrashGuardRecord{mqtt_config::CRASH_GUARD_MAGIC, streak, 1, {0, 0}};
}

static std::vector<std::string> topics(size_t begin, size_t end) {
  std::vector<std::string> out;
  for (size_t i = begin; i < end; i++)
    out.push_back("t" + std::to_string(i));
  return out;
}

// Nine slots on topics t0…t8, three of each kind: waves of four, four and one.
class SubscribingTest : public SlotsTest {
 protected:
  TestSubscriptions &boot_nine(uint32_t wave_timeout_ms = 60000) {
    const SlotKind kinds[] = {SlotKind::SENSOR, SlotKind::BINARY_SENSOR, SlotKind::TEXT_SENSOR};
    std::vector<SlotConfig> slots;
    for (size_t i = 0; i < 9; i++)
      slots.push_back(slot_of(("S" + std::to_string(i)).c_str(), ("t" + std::to_string(i)).c_str(), kinds[i % 3]));
    this->plant(slots);
    TestSubscriptions &s = this->boot([wave_timeout_ms](TestSubscriptions &before) {
      before.set_max_slots(9);
      before.set_wave_timeout(wave_timeout_ms);
    });
    this->client->subscribe(COMMAND, [this](const std::string &, const std::string &) { this->commands++; });
    return s;
  }

  void tick() { App.scheduler.call(millis()); }
  void wait_ms(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    this->tick();
  }
  // Our SUBSCRIBEs, in order: everything but the command topic.
  std::vector<std::string> ours() const {
    std::vector<std::string> out;
    for (const std::string &topic : this->client->sent_subscribes) {
      if (topic != COMMAND)
        out.push_back(topic);
    }
    return out;
  }
  size_t ours_in_the_list() const {
    size_t count = 0;
    for (const std::string &topic : topics(0, 9))
      count += this->subscriptions_to(topic);
    return count;
  }

  int commands{0};
};

TEST_F(SubscribingTest, TheClientsOwnListGoesFirstThenOursFourAtATime) {
  EXPECT_EQ(MqttSubscriptions::SUBSCRIBE_WAVE, 4u);
  this->boot_nine();
  EXPECT_EQ(this->ours_in_the_list(), 0u);
  this->client->connect_for_test();
  EXPECT_EQ(this->client->sent_subscribes, std::vector<std::string>{COMMAND});
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 4));

  // A wave is in once each topic has had its message; a second one of the same topic is not
  // another topic's.
  this->deliver("t0", "1");
  this->deliver("t0", "2");
  this->deliver("t1", "ON");
  this->deliver("t2", "x");
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 4));
  this->deliver("t3", "1");
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 8));
  for (const std::string &topic : topics(4, 8))
    this->deliver(topic, "1");
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 9));

  // The last wave's answer sends nothing more, and each topic is in the list once.
  this->deliver("t8", "1");
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 9));
  EXPECT_EQ(this->ours_in_the_list(), 9u);
  EXPECT_EQ(this->subscriptions_to(COMMAND), 1u);
}

// A topic with no retained value never answers: its wave is in after the timeout.
TEST_F(SubscribingTest, UnansweredTheNextWaveWaitsForTheTimeout) {
  this->boot_nine(200);
  this->client->connect_for_test();
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 4));
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 4));
  this->wait_ms(250);
  EXPECT_EQ(this->ours(), topics(0, 8));
  this->wait_ms(250);
  EXPECT_EQ(this->ours(), topics(0, 9));
  this->wait_ms(250);
  EXPECT_EQ(this->ours(), topics(0, 9));
}

// A first answer restarts the wait, a repeated one does not: chatter on one topic cannot hold
// back the rest.
TEST_F(SubscribingTest, AFirstAnswerRestartsTheTimeout) {
  this->boot_nine(1000);
  this->client->connect_for_test();
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 4));
  this->wait_ms(500);
  ASSERT_EQ(this->ours(), topics(0, 4));
  this->deliver("t0", "1");  // due now at +1500
  this->wait_ms(700);
  EXPECT_EQ(this->ours(), topics(0, 4));
  this->deliver("t0", "2");
  this->wait_ms(500);
  EXPECT_EQ(this->ours(), topics(0, 8));
}

// The client sends its whole list again on every connect; ours are not in it by then.
TEST_F(SubscribingTest, AReconnectStartsOverAndOursNeverRideTheClientsBurst) {
  this->boot_nine();
  this->online();
  EXPECT_EQ(this->ours_in_the_list(), 9u);
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  EXPECT_EQ(this->ours_in_the_list(), 0u);
  EXPECT_EQ(this->subscriptions_to(COMMAND), 1u);

  this->client->sent_subscribes.clear();
  this->client->connect_for_test();
  EXPECT_EQ(this->client->sent_subscribes, std::vector<std::string>{COMMAND});
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 4));
  this->deliver("t1", "ON");
  EXPECT_EQ(this->subs->binary(1)->state, true);
}

TEST_F(SubscribingTest, ADisconnectMidWaveStopsTheWaves) {
  this->boot_nine(30);
  this->client->connect_for_test();
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 4));
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  EXPECT_EQ(this->ours_in_the_list(), 0u);
  this->wait_ms(60);
  this->wait_ms(60);
  EXPECT_EQ(this->ours(), topics(0, 4));

  this->client->sent_subscribes.clear();
  this->client->connect_for_test();
  this->tick();
  EXPECT_EQ(this->ours(), topics(0, 4));
}

// esp-mqtt may report a reconnect whose disconnect never reached the loop.
TEST_F(SubscribingTest, AConnectWithNoDisconnectBeforeItLeavesOursInOnce) {
  this->boot_nine();
  this->online();
  this->client->sent_subscribes.clear();
  this->client->connect_for_test();
  EXPECT_EQ(this->client->sent_subscribes, std::vector<std::string>{COMMAND});
  this->subs->subscribe_all();
  EXPECT_EQ(this->ours(), topics(0, 9));
  for (const std::string &topic : topics(0, 9))
    EXPECT_EQ(this->subscriptions_to(topic), 1u) << topic;
}

// esp-mqtt reconnects on its own; the client calls the connection its own only once its state
// machine is back.
TEST_F(SubscribingTest, TheWavesWaitForTheClientToTakeTheConnection) {
  this->boot_nine(30);
  this->client->backend_connect_for_test();
  this->tick();
  EXPECT_TRUE(this->client->sent_subscribes.empty());
  this->wait_ms(60);
  EXPECT_TRUE(this->client->sent_subscribes.empty());
  this->client->take_connection_for_test();
  EXPECT_EQ(this->client->sent_subscribes, std::vector<std::string>{COMMAND});
  this->wait_ms(60);
  EXPECT_EQ(this->ours(), topics(0, 4));
}

// Ours are told apart by their callback, not their topic.
TEST_F(SubscribingTest, ASlotOnACommandTopicLeavesTheCommandSubscriptionAlone) {
  this->plant({slot_of("Mirror", COMMAND, SlotKind::TEXT_SENSOR)});
  TestSubscriptions &s = this->boot();
  this->client->subscribe(COMMAND, [this](const std::string &, const std::string &) { this->commands++; });
  this->online();
  EXPECT_EQ(this->subscriptions_to(COMMAND), 2u);
  this->deliver(COMMAND, "ON");
  EXPECT_EQ(s.text(0)->state, "ON");
  EXPECT_EQ(this->commands, 1);
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  EXPECT_EQ(this->subscriptions_to(COMMAND), 1u);
  this->deliver(COMMAND, "OFF");
  EXPECT_EQ(s.text(0)->state, "ON");
  EXPECT_EQ(this->commands, 2);
}

TEST_F(SubscribingTest, SuspendedSlotsAreNeverSubscribed) {
  this->plant({slot_of("Outdoor", "t")});
  this->board.rtc = armed_streak(1);
  this->board.panic = true;
  TestSubscriptions &s = this->boot();
  ASSERT_TRUE(s.suspended());
  for (int i = 0; i < 2; i++) {
    this->client->connect_for_test();
    this->tick();
    this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  }
  EXPECT_TRUE(this->client->sent_subscribes.empty());
  EXPECT_TRUE(this->client->subscriptions().empty());
}

}  // namespace esphome::mqtt_subscriptions::testing
