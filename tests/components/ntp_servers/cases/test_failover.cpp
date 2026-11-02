#include "common.h"

namespace esphome::ntp_servers::testing {

TEST_F(Servers, AServerThatDoesNotAnswerMakesWayForTheNext) {
  TestServers &servers = this->boot();
  this->pass(ANSWER_MS - 1);
  EXPECT_EQ(servers.handed, (Names{POOL_0}));
  this->pass(1);
  EXPECT_EQ(servers.handed, (Names{POOL_0, POOL_1}));
  EXPECT_TRUE(LogCapture::instance().has("No answer from 0.pool.ntp.org; asking 1.pool.ntp.org"));
  this->pass(ANSWER_MS);
  this->pass(ANSWER_MS);
  EXPECT_EQ(servers.handed, (Names{POOL_0, POOL_1, POOL_2, POOL_0}));
}

// With nobody answering, a whole round in each wait doubles, up to a poll.
TEST_F(Servers, ARoundWithoutAnAnswerWaitsLonger) {
  TestServers &servers = this->boot();
  for (int i = 0; i < 3; i++)
    this->pass(ANSWER_MS);
  ASSERT_EQ(servers.handed.size(), 4u);
  this->pass(2 * ANSWER_MS - 1);
  EXPECT_EQ(servers.handed.size(), 4u);
  this->pass(1);
  EXPECT_EQ(servers.handed.size(), 5u);

  // 30 s doubled five times is past the 15 min poll, which is as long as it gets.
  for (int i = 0; i < 20; i++)
    this->pass(POLL_MS);
  const size_t asked = servers.handed.size();
  this->pass(POLL_MS - 1);
  EXPECT_EQ(servers.handed.size(), asked);
  this->pass(1);
  EXPECT_EQ(servers.handed.size(), asked + 1);
}

TEST_F(Servers, AnAnswerIsRecordedWithItsServer) {
  TestServers &servers = this->boot();
  this->pass(ANSWER_MS);
  servers.now = JULY + 31;
  this->answer();
  ASSERT_TRUE(servers.last_sync().has_value());
  EXPECT_EQ(servers.last_sync()->time, JULY + 31);
  EXPECT_EQ(servers.last_sync()->server, POOL_1);
  EXPECT_STREQ(servers.current(), POOL_1);
}

// lwIP polls the server that answered; while it keeps answering nothing changes.
TEST_F(Servers, TheFirstIsKeptWhileItAnswers) {
  TestServers &servers = this->boot();
  this->answer();
  for (int i = 0; i < 4; i++) {
    this->pass(POLL_MS);
    this->answer();
  }
  this->pass(POLL_MS + ANSWER_MS - 1);
  EXPECT_EQ(servers.handed, (Names{POOL_0}));
  // A poll missed: the next server.
  this->pass(1);
  EXPECT_EQ(servers.handed, (Names{POOL_0, POOL_1}));
}

TEST_F(Servers, AfterAnotherAnswersTheFirstIsAskedAtThePoll) {
  TestServers &servers = this->boot();
  ASSERT_EQ(servers.configure({"ntp.local"}, true), SetError::NONE);
  this->pass(ANSWER_MS);
  ASSERT_STREQ(servers.asked(), POOL_0);
  this->answer();
  EXPECT_EQ(servers.last_sync()->server, POOL_0);
  // Ahead of lwIP's own poll, which would ask this one again first.
  this->pass(POLL_MS - RETURN_LEAD_MS - 1);
  EXPECT_STREQ(servers.asked(), POOL_0);
  this->pass(1);
  EXPECT_STREQ(servers.asked(), "ntp.local");
  // Back, it answers: it stays.
  this->answer();
  EXPECT_EQ(servers.last_sync()->server, "ntp.local");
  this->pass(POLL_MS);
  EXPECT_STREQ(servers.asked(), "ntp.local");
}

// Still down at that poll, the round goes on from the first as before.
TEST_F(Servers, TheFirstStillDownAtThePollHandsOnAgain) {
  TestServers &servers = this->boot();
  ASSERT_EQ(servers.configure({"ntp.local"}, true), SetError::NONE);
  this->pass(ANSWER_MS);
  this->answer();
  this->pass(POLL_MS);
  ASSERT_STREQ(servers.asked(), "ntp.local");
  this->pass(ANSWER_MS);
  EXPECT_STREQ(servers.asked(), POOL_0);
  EXPECT_EQ(servers.last_sync()->server, POOL_0);
}

// Offline nothing can answer, so no server is given up on; back online the first is asked again.
TEST_F(Servers, OfflineTimeDoesNotCount) {
  TestServers &servers = this->boot();
  this->pass(ANSWER_MS);
  this->pass(ANSWER_MS);
  ASSERT_STREQ(servers.asked(), POOL_2);
  servers.online = false;
  this->pass(10 * POLL_MS);
  EXPECT_EQ(servers.handed.size(), 3u);
  servers.online = true;
  this->pass(0);
  EXPECT_EQ(servers.handed, (Names{POOL_0, POOL_1, POOL_2, POOL_0}));
  // A fresh round: the first wait again.
  this->pass(ANSWER_MS);
  EXPECT_STREQ(servers.asked(), POOL_1);
}

// Booted offline, as the device always is: lwIP is restarted once the network is up.
TEST_F(Servers, TheNetworkComingUpAsksTheFirst) {
  auto device = std::make_unique<Device>();
  device->servers.online = false;
  device->clock.set_update_interval(POLL_MS);
  device->servers.set_clock(&device->clock);
  device->servers.add_fallback_server(POOL_0);
  device->servers.add_fallback_server(POOL_1);
  device->servers.set_preference_hash(PREF_HASH);
  device->servers.setup();
  this->devices.push_back(std::move(device));
  TestServers &servers = this->servers();
  this->pass(ANSWER_MS);
  EXPECT_EQ(servers.handed, (Names{POOL_0}));
  servers.online = true;
  this->pass(0);
  EXPECT_EQ(servers.handed, (Names{POOL_0, POOL_0}));
  this->pass(0);
  EXPECT_EQ(servers.handed.size(), 2u);
}

// One server is lwIP's to retry, at its own pace.
TEST_F(Servers, ALoneServerIsNeverHandedAgain) {
  TestServers &servers = this->boot();
  ASSERT_EQ(servers.configure({"ntp.local"}, false), SetError::NONE);
  for (int i = 0; i < 10; i++)
    this->pass(POLL_MS);
  this->answer();
  this->pass(2 * POLL_MS);
  EXPECT_EQ(servers.handed, (Names{POOL_0, "ntp.local"}));
  EXPECT_EQ(servers.last_sync()->server, "ntp.local");
}

TEST_F(Servers, AChangeStartsAFreshRoundFromTheFirst) {
  TestServers &servers = this->boot();
  this->pass(ANSWER_MS);
  this->pass(ANSWER_MS);
  this->pass(ANSWER_MS);
  ASSERT_STREQ(servers.asked(), POOL_0);
  ASSERT_EQ(servers.configure({"ntp.local"}, true), SetError::NONE);
  EXPECT_STREQ(servers.asked(), "ntp.local");
  this->pass(ANSWER_MS);
  EXPECT_STREQ(servers.asked(), POOL_0);
}

// Upstream's first loop pass reports a clock already set, as it is after a restart: no NTP
// server answered.
TEST_F(Servers, ASyncWithoutAReplyIsNoAnswer) {
  TestServers &servers = this->boot();
  this->device().clock.sync();
  EXPECT_FALSE(servers.last_sync().has_value());
  this->pass(ANSWER_MS);
  EXPECT_STREQ(servers.asked(), POOL_1);
}

// A reply the loop task hears of only after the device moved on is the old server's.
TEST_F(Servers, AReplyFromAServerNoLongerAskedMovesNothing) {
  TestServers &servers = this->boot();
  servers.reply_from(POOL_0);
  ASSERT_EQ(servers.configure({"ntp.local"}, true), SetError::NONE);
  this->device().clock.sync();
  ASSERT_TRUE(servers.last_sync().has_value());
  EXPECT_EQ(servers.last_sync()->server, POOL_0);
  EXPECT_STREQ(servers.current(), "ntp.local");
  // Still the first wait for the one asked now, not a poll.
  this->pass(ANSWER_MS);
  EXPECT_STREQ(servers.asked(), POOL_0);
}

TEST_F(Servers, AReplyIsCountedOnce) {
  TestServers &servers = this->boot();
  this->answer();
  servers.now = JULY + 60;
  this->device().clock.sync();
  EXPECT_EQ(servers.last_sync()->time, JULY);
}

TEST_F(Servers, TheLastAnswerOutlivesAChange) {
  TestServers &servers = this->boot();
  this->answer();
  ASSERT_EQ(servers.configure({"ntp.local"}, false), SetError::NONE);
  ASSERT_TRUE(servers.last_sync().has_value());
  EXPECT_EQ(servers.last_sync()->server, POOL_0);
  EXPECT_STREQ(servers.current(), "ntp.local");
}

TEST_F(Servers, TheWaitsCountRightPastTheMillisWrap) {
  TestServers &servers = this->boot({POOL_0, POOL_1, POOL_2}, UINT32_MAX - ANSWER_MS / 2);
  this->pass(ANSWER_MS - 1);
  EXPECT_EQ(servers.handed, (Names{POOL_0}));
  this->pass(1);
  EXPECT_EQ(servers.handed, (Names{POOL_0, POOL_1}));
}

// ESP-IDF polls every 15 s at the most: a server still gets its whole wait, a round in too.
TEST_F(Servers, APollShorterThanTheWaitLeavesTheWait) {
  auto device = std::make_unique<Device>();
  device->clock.set_update_interval(1000);
  device->servers.set_clock(&device->clock);
  device->servers.add_fallback_server(POOL_0);
  device->servers.add_fallback_server(POOL_1);
  device->servers.set_preference_hash(PREF_HASH);
  device->servers.setup();
  this->devices.push_back(std::move(device));
  TestServers &servers = this->servers();
  for (size_t i = 1; i < 5; i++) {
    this->pass(ANSWER_MS - 1);
    EXPECT_EQ(servers.handed.size(), i);
    this->pass(1);
    EXPECT_EQ(servers.handed.size(), i + 1);
  }
}

}  // namespace esphome::ntp_servers::testing
