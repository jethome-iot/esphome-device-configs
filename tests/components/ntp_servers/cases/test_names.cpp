#include "common.h"

namespace esphome::ntp_servers::testing {

TEST(ServerName, HostNamesAndAddressesAreTaken) {
  for (const char *name : {"pool.ntp.org", "ntp", "time-1.example.com", "NTP.Example.COM", "a.b", "123abc", "10.0.0.1x",
                           "1.2.3.4", "192.168.0.255", "0.0.0.0", "255.255.255.255"}) {
    EXPECT_EQ(check_server(name), SetError::NONE) << name;
  }
}

TEST(ServerName, WhatNoLookupCouldResolveIsRefused) {
  for (const char *name : {"", ".", "a..b", ".a", "a.", "-a", "a-", "a.-b", "a_b", "a b", " a", "a:123", "[::1]", "::1",
                           "ntp.example.com/", "время.рф"}) {
    EXPECT_EQ(check_server(name), SetError::BAD_NAME) << name;
  }
  EXPECT_EQ(check_server(std::string("ntp\0.local", 10)), SetError::BAD_NAME);
}

// Digits alone read as an address to lwIP, never as a name to look up.
TEST(ServerName, AllDigitsMustBeAnIpv4Address) {
  for (const char *name : {"1234", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.1000", "01.2.3.4", "1.2.3.00"}) {
    EXPECT_EQ(check_server(name), SetError::BAD_NAME) << name;
  }
}

TEST(ServerName, SixtyThreeBytesAreTheMost) {
  const std::string longest = std::string(31, 'a') + "." + std::string(31, 'b');
  ASSERT_EQ(longest.size(), SERVER_NAME_MAX);
  EXPECT_EQ(check_server(longest), SetError::NONE);
  EXPECT_EQ(check_server(longest + "c"), SetError::TOO_LONG);
  // Before what it holds.
  EXPECT_EQ(check_server(std::string(64, '_')), SetError::TOO_LONG);
}

TEST(ServerList, UpToThreeDifferentServers) {
  size_t item = 9, first = 9;
  EXPECT_EQ(NTPServers::check_servers({}, item, first), SetError::NONE);
  EXPECT_EQ(NTPServers::check_servers({"a.example", "b.example", "10.0.0.1"}, item, first), SetError::NONE);
  EXPECT_EQ(NTPServers::check_servers({"a", "b", "c", "d"}, item, first), SetError::TOO_MANY);
}

TEST(ServerList, TheEntryAtFaultIsNamed) {
  size_t item = 9, first = 9;
  EXPECT_EQ(NTPServers::check_servers({"a.example", "a_b", "-"}, item, first), SetError::BAD_NAME);
  EXPECT_EQ(item, 1u);
  EXPECT_EQ(NTPServers::check_servers({"a", std::string(64, 'a')}, item, first), SetError::TOO_LONG);
  EXPECT_EQ(item, 1u);
  // Case aside: DNS reads them as one.
  EXPECT_EQ(NTPServers::check_servers({"a.example", "b.example", "A.Example"}, item, first), SetError::REPEATED);
  EXPECT_EQ(item, 2u);
  EXPECT_EQ(first, 0u);
}

TEST(ServerList, RefusalsSayWhatTheApiWasSent) {
  EXPECT_EQ(set_error_text(SetError::TOO_MANY), "'ntp.servers' lists more than 3 servers");
  EXPECT_EQ(set_error_text(SetError::TOO_LONG, 1), "'ntp.servers' item 2 is over 63 bytes");
  EXPECT_EQ(set_error_text(SetError::BAD_NAME, 0), "'ntp.servers' item 1 is not a host name or an IPv4 address");
  EXPECT_EQ(set_error_text(SetError::REPEATED, 2, 0), "'ntp.servers' item 3 repeats item 1");
  EXPECT_EQ(set_error_text(SetError::NOT_STORED), "The NTP servers were not stored");
  EXPECT_EQ(set_error_text(SetError::NONE), "");
}

}  // namespace esphome::ntp_servers::testing
