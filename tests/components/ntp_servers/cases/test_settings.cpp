#include "common.h"
#include <cstring>

namespace esphome::ntp_servers::testing {

// As before there was a choice: the clock's own servers, the first one first.
TEST_F(Servers, NothingStoredAsksTheClocksOwn) {
  TestServers &servers = this->boot();
  EXPECT_TRUE(servers.servers().empty());
  EXPECT_TRUE(servers.fallback());
  EXPECT_EQ(servers.in_use(), (Names{POOL_0, POOL_1, POOL_2}));
  EXPECT_EQ(servers.handed, (Names{POOL_0}));
  EXPECT_STREQ(servers.current(), POOL_0);
  EXPECT_FALSE(servers.last_sync().has_value());
}

TEST_F(Servers, SetServersGoFirstAndAreAskedAtOnce) {
  TestServers &servers = this->boot();
  ASSERT_EQ(servers.configure({"ntp.local", "10.0.0.1"}, true), SetError::NONE);
  EXPECT_EQ(servers.stores, 1);
  EXPECT_EQ(servers.in_use(), (Names{"ntp.local", "10.0.0.1", POOL_0, POOL_1, POOL_2}));
  EXPECT_STREQ(servers.asked(), "ntp.local");
  EXPECT_STREQ(servers.current(), "ntp.local");
}

TEST_F(Servers, TheNextBootComesUpOnThem) {
  ASSERT_EQ(this->boot().configure({"ntp.local", "10.0.0.1"}, false), SetError::NONE);
  TestServers &servers = this->boot();
  EXPECT_EQ(servers.servers(), (Names{"ntp.local", "10.0.0.1"}));
  EXPECT_FALSE(servers.fallback());
  EXPECT_EQ(servers.in_use(), (Names{"ntp.local", "10.0.0.1"}));
  EXPECT_EQ(servers.handed, (Names{"ntp.local"}));
}

// DNS reads a name the same whatever its case.
TEST_F(Servers, AFallbackServerAlreadySetIsAskedOnce) {
  TestServers &servers = this->boot();
  ASSERT_EQ(servers.configure({"1.POOL.ntp.org", "ntp.local"}, true), SetError::NONE);
  EXPECT_EQ(servers.in_use(), (Names{"1.POOL.ntp.org", "ntp.local", POOL_0, POOL_2}));
}

TEST_F(Servers, WithTheFallbackOffOnlyTheSetOnesAreAsked) {
  TestServers &servers = this->boot();
  ASSERT_EQ(servers.configure({"ntp.local"}, false), SetError::NONE);
  EXPECT_EQ(servers.in_use(), (Names{"ntp.local"}));
  // Back on, the clock's own follow again.
  ASSERT_EQ(servers.configure({"ntp.local"}, true), SetError::NONE);
  EXPECT_EQ(servers.in_use(), (Names{"ntp.local", POOL_0, POOL_1, POOL_2}));
}

TEST_F(Servers, NothingToAskStopsLwip) {
  TestServers &servers = this->boot();
  ASSERT_EQ(servers.configure({}, false), SetError::NONE);
  EXPECT_TRUE(servers.in_use().empty());
  EXPECT_EQ(servers.current(), nullptr);
  EXPECT_STREQ(servers.asked(), "");

  // An answer already on its way when it stopped is nobody's.
  this->answer();
  EXPECT_FALSE(servers.last_sync().has_value());
  servers.online = false;
  this->pass(POLL_MS);
  servers.online = true;
  this->pass(POLL_MS);
  EXPECT_EQ(servers.handed.size(), 2u);

  EXPECT_EQ(this->boot().handed, (Names{""}));
}

TEST_F(Servers, AFirmwareWithoutServersOfItsOwnAsksOnlyTheSetOnes) {
  TestServers &servers = this->boot({});
  EXPECT_TRUE(servers.in_use().empty());
  EXPECT_EQ(servers.handed, (Names{""}));
  ASSERT_EQ(servers.configure({"ntp.local"}, true), SetError::NONE);
  EXPECT_EQ(servers.in_use(), (Names{"ntp.local"}));
}

TEST_F(Servers, TheSameServersAgainChangeNothing) {
  TestServers &servers = this->boot();
  ASSERT_EQ(servers.configure({"ntp.local"}, true), SetError::NONE);
  ASSERT_EQ(servers.configure({"ntp.local"}, true), SetError::NONE);
  EXPECT_EQ(servers.stores, 1);
  EXPECT_EQ(servers.handed, (Names{POOL_0, "ntp.local"}));
}

TEST_F(Servers, NothingSetWithTheFallbackOnIsWhatNothingStoredMeans) {
  TestServers &servers = this->boot();
  ASSERT_EQ(servers.configure({}, true), SetError::NONE);
  EXPECT_EQ(servers.stores, 0);
  EXPECT_EQ(servers.handed, (Names{POOL_0}));
}

TEST_F(Servers, ARefusedListChangesNothing) {
  TestServers &servers = this->boot();
  EXPECT_EQ(servers.configure({"a", "b", "c", "d"}, true), SetError::TOO_MANY);
  EXPECT_EQ(servers.configure({"ntp.local", "bad_name"}, false), SetError::BAD_NAME);
  EXPECT_EQ(servers.configure({"ntp.local", "NTP.local"}, false), SetError::REPEATED);
  EXPECT_EQ(servers.stores, 0);
  EXPECT_TRUE(servers.servers().empty());
  EXPECT_TRUE(servers.fallback());
  EXPECT_EQ(servers.handed, (Names{POOL_0}));
}

TEST_F(Servers, AStoreThatFailsLeavesTheServersInUse) {
  TestServers &servers = this->boot();
  servers.flush = TestServers::Flush::FAILED_RECORD_LOST;
  EXPECT_EQ(servers.configure({"ntp.local"}, false), SetError::NOT_STORED);
  EXPECT_TRUE(servers.servers().empty());
  EXPECT_EQ(servers.in_use(), (Names{POOL_0, POOL_1, POOL_2}));
  EXPECT_EQ(servers.handed, (Names{POOL_0}));
  EXPECT_TRUE(LogCapture::instance().has("Storing the NTP servers failed"));
  EXPECT_TRUE(this->boot().servers().empty());
}

// The flush reports for every record: one that reached flash all the same is in force.
TEST_F(Servers, AFlushThatFailedForAnotherRecordStillSets) {
  TestServers &servers = this->boot();
  servers.flush = TestServers::Flush::FAILED_RECORD_LANDED;
  EXPECT_EQ(servers.configure({"ntp.local"}, false), SetError::NONE);
  EXPECT_EQ(servers.in_use(), (Names{"ntp.local"}));
  EXPECT_TRUE(LogCapture::instance().has("the NTP servers are stored all the same"));
  EXPECT_EQ(this->boot().servers(), (Names{"ntp.local"}));
}

// Another firmware may have taken what this one refuses: each is checked as it is read.
TEST_F(Servers, AStoredServerThisFirmwareRefusesIsDropped) {
  StoredServers stored{};
  stored.fallback = 0;
  std::strcpy(stored.servers[0], "ntp.local");
  std::strcpy(stored.servers[1], "bad_name");
  std::strcpy(stored.servers[2], "NTP.LOCAL");
  this->store_raw(stored);
  TestServers &servers = this->boot();
  EXPECT_EQ(servers.servers(), (Names{"ntp.local"}));
  EXPECT_FALSE(servers.fallback());
  EXPECT_TRUE(LogCapture::instance().has("The stored server 'bad_name' is refused; dropped"));
  EXPECT_TRUE(LogCapture::instance().has("The stored server 'NTP.LOCAL' is refused; dropped"));
}

// A name that fills its slot reads back whole, and one without its NUL no further than the slot.
TEST_F(Servers, AStoredNameEndsWithItsSlot) {
  StoredServers stored{};
  stored.fallback = 1;
  const std::string longest = std::string(31, 'a') + "." + std::string(31, 'b');
  std::memcpy(stored.servers[0], longest.data(), longest.size());
  std::memset(stored.servers[1], 'c', sizeof(stored.servers[1]));
  this->store_raw(stored);
  TestServers &servers = this->boot();
  EXPECT_EQ(servers.servers(), (Names{longest, std::string(SERVER_NAME_MAX, 'c')}));
}

TEST_F(Servers, DumpConfigSaysWhatIsAsked) {
  TestServers &servers = this->boot();
  ASSERT_EQ(servers.configure({"ntp.local", "10.0.0.1"}, true), SetError::NONE);
  LogCapture::instance().clear();
  servers.dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Set: ntp.local, 10.0.0.1")) << LogCapture::instance().lines.size();
  EXPECT_TRUE(LogCapture::instance().has("Fallback: YES (0.pool.ntp.org, 1.pool.ntp.org, 2.pool.ntp.org)"));
  EXPECT_TRUE(LogCapture::instance().has("Asking: ntp.local"));

  ASSERT_EQ(servers.configure({}, false), SetError::NONE);
  LogCapture::instance().clear();
  servers.dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Set: none"));
  EXPECT_TRUE(LogCapture::instance().has("Fallback: NO"));
  EXPECT_TRUE(LogCapture::instance().has("Asking: none"));
}

}  // namespace esphome::ntp_servers::testing
