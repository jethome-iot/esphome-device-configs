#include "common.h"

namespace esphome::network_config::testing {

TEST(Ip4, TakesACanonicalDottedQuad) {
  for (const char *text : {"0.0.0.0", "1.2.3.4", "10.0.2.15", "192.168.1.50", "255.255.255.255"})
    EXPECT_TRUE(is_ip4(text)) << text;
}

TEST(Ip4, ReadsTheOctetsInOrder) {
  uint32_t address = 0;
  ASSERT_TRUE(parse_ip4("10.0.2.15", address));
  EXPECT_EQ(address, 0x0A00020FU);
}

TEST(Ip4, RefusesTheShortHexAndOctalFormsAtonWouldTake) {
  // inet_aton reads "10.1" as 10.0.0.1, "0x0a000001" as hex and a leading zero as octal.
  for (const char *text : {"10", "10.1", "10.0.1", "0x0a000001", "010.0.0.1", "1.2.3.04"})
    EXPECT_FALSE(is_ip4(text)) << text;
}

TEST(Ip4, RefusesOutOfRangeAndMalformedOctets) {
  for (const char *text : {"1.2.3.256", "256.0.0.1", "1.2.3.4.5", "1.2.3.", ".1.2.3", "1.2..4", "1.2.3.-1", "1.2.3.a"})
    EXPECT_FALSE(is_ip4(text)) << text;
}

TEST(Ip4, RefusesEmptinessAndAnythingAroundTheAddress) {
  for (const char *text : {"", " ", " 1.2.3.4", "1.2.3.4 ", "1.2.3.4\n", "1.2.3.4:80", "1.2.3.4/24"})
    EXPECT_FALSE(is_ip4(text)) << text;
}

TEST(Ip4, RefusesIPv6) {
  // Both ManualIP structs are IPv4-only; an IPv6 value would take four wrong bytes.
  for (const char *text : {"::1", "fe80::1", "::ffff:10.0.0.1"})
    EXPECT_FALSE(is_ip4(text)) << text;
}

TEST(Ip4, ZeroIsAnAddressLikeAnyOther) {
  // "unset" is the empty string in the record, never 0.0.0.0.
  uint32_t address = 1;
  EXPECT_TRUE(parse_ip4("0.0.0.0", address));
  EXPECT_EQ(address, 0U);
}

TEST(Ip4, FormatsBackWhatItParsed) {
  for (const char *text : {"0.0.0.0", "1.2.3.4", "10.0.2.15", "255.255.255.255"}) {
    uint32_t address = 0;
    ASSERT_TRUE(parse_ip4(text, address)) << text;
    EXPECT_EQ(format_ip4(address), text);
  }
}

}  // namespace esphome::network_config::testing
