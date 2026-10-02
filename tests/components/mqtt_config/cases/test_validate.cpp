#include "common.h"

namespace esphome::mqtt_config::testing {

// The messages are the dashboard's too (client/device/mqttRules.ts, MQTT_MESSAGES): verbatim.
static const char *const BROKER_REQUIRED = "'broker' is required to turn MQTT on";
static const char *const BROKER_TOO_LONG = "'broker' is over 128 characters";
static const char *const BROKER_URL = "'broker' takes a host name, not a URL: drop the 'mqtt://'";
static const char *const BROKER_PORT = "'broker' cannot carry a port: put it in 'port'";
static const char *const BROKER_CHARS = "'broker' must be a host name or an IPv4 address";
static const char *const PORT_RANGE = "'port' must be between 1 and 65535";
static const char *const USERNAME_TOO_LONG = "'username' is over 64 bytes";
static const char *const USERNAME_TEXT = "'username' must be text without control characters";
static const char *const PASSWORD_TOO_LONG = "'password' is over 128 bytes";
static const char *const PASSWORD_NUL = "'password' cannot contain a NUL character";
static const char *const PASSWORD_NEEDS_USERNAME = "'password' needs a 'username': MQTT 3.1.1 sends none without one";
static const char *const CLIENT_ID_TOO_LONG = "'client_id' is over 64 characters";
static const char *const CLIENT_ID_CHARS = "'client_id' must be printable ASCII without spaces";
static const char *const PREFIX_TOO_LONG = "'topic_prefix' is over 64 bytes";
static const char *const PREFIX_WILDCARD = "'topic_prefix' cannot contain '+' or '#'";
static const char *const PREFIX_TEXT = "'topic_prefix' must be text without control characters";
static const char *const PREFIX_DOLLAR = "'topic_prefix' cannot start with '$'";
static const char *const PREFIX_SLASH = "'topic_prefix' cannot end with '/'";

static std::string why(const MqttRecord &record, bool ipv6 = false) {
  const char *error = validate(record, ipv6);
  return error == nullptr ? "" : error;
}

static MqttRecord valid() {
  MqttRecord r = enabled_record("broker.local");
  r.username = "jxd";
  r.password = "pw";
  r.client_id = "jxd-1";
  r.topic_prefix = "home/jxd";
  return r;
}

TEST(MqttValidate, AFullRecordPasses) { EXPECT_EQ(why(valid()), ""); }

TEST(MqttValidate, TheDefaultsPass) { EXPECT_EQ(why(MqttRecord{}), ""); }

// Off with nothing set is a valid state: a fresh device is exactly that.
TEST(MqttValidate, ADisabledRecordWithoutABrokerPasses) {
  MqttRecord r = valid();
  r.enabled = false;
  r.broker = "";
  EXPECT_EQ(why(r), "");
}

// One case per rule, each breaking only that one, in the order validate() checks them.
TEST(MqttValidate, EveryRuleHasItsMessage) {
  struct Case {
    const char *expected;
    std::function<void(MqttRecord &)> breaks;
  };
  const std::vector<Case> cases = {
      {BROKER_REQUIRED, [](MqttRecord &r) { r.broker = ""; }},
      {BROKER_TOO_LONG, [](MqttRecord &r) { r.broker = std::string(129, 'b'); }},
      {BROKER_URL, [](MqttRecord &r) { r.broker = "mqtt://broker"; }},
      {BROKER_PORT, [](MqttRecord &r) { r.broker = "broker:1883"; }},
      {BROKER_CHARS, [](MqttRecord &r) { r.broker = "my broker"; }},
      {PORT_RANGE, [](MqttRecord &r) { r.port = 0; }},
      {USERNAME_TOO_LONG, [](MqttRecord &r) { r.username = std::string(65, 'u'); }},
      {USERNAME_TEXT, [](MqttRecord &r) { r.username = "j\txd"; }},
      {PASSWORD_TOO_LONG, [](MqttRecord &r) { r.password = std::string(129, 'p'); }},
      {PASSWORD_NUL, [](MqttRecord &r) { r.password = std::string("p\0w", 3); }},
      {PASSWORD_NEEDS_USERNAME, [](MqttRecord &r) { r.username = ""; }},
      {CLIENT_ID_TOO_LONG, [](MqttRecord &r) { r.client_id = std::string(65, 'c'); }},
      {CLIENT_ID_CHARS, [](MqttRecord &r) { r.client_id = "jxd 1"; }},
      {PREFIX_TOO_LONG, [](MqttRecord &r) { r.topic_prefix = std::string(65, 't'); }},
      {PREFIX_WILDCARD, [](MqttRecord &r) { r.topic_prefix = "home/+"; }},
      {PREFIX_TEXT, [](MqttRecord &r) { r.topic_prefix = "home\x7f"; }},
      {PREFIX_DOLLAR, [](MqttRecord &r) { r.topic_prefix = "$SYS"; }},
      {PREFIX_SLASH, [](MqttRecord &r) { r.topic_prefix = "home/"; }},
  };
  for (const Case &c : cases) {
    MqttRecord r = valid();
    c.breaks(r);
    EXPECT_EQ(why(r), c.expected);
  }
}

TEST(MqttValidate, TheFirstBrokenRuleIsTheAnswer) {
  MqttRecord r = valid();
  r.broker = "mqtt://a b";
  r.port = 0;
  EXPECT_EQ(why(r), BROKER_URL);
  r.broker = "broker";
  EXPECT_EQ(why(r), PORT_RANGE);
  r.port = 1883;
  r.username = std::string(65, 'u');
  r.password = std::string(129, 'p');
  EXPECT_EQ(why(r), USERNAME_TOO_LONG);
  r.username = "jxd";
  EXPECT_EQ(why(r), PASSWORD_TOO_LONG);
  r.password = "pw";
  r.topic_prefix = "$a+";
  EXPECT_EQ(why(r), PREFIX_WILDCARD);
  r.topic_prefix = "$a/";
  EXPECT_EQ(why(r), PREFIX_DOLLAR);
}

// Bytes, not characters, for the text fields; the broker is ASCII, so the two are one there.
TEST(MqttValidate, TheLimitsAreInclusive) {
  MqttRecord r = valid();
  r.broker = std::string(128, 'b');
  r.username = std::string(64, 'u');
  r.password = std::string(128, 'p');
  r.client_id = std::string(64, 'c');
  r.topic_prefix = std::string(64, 't');
  EXPECT_EQ(why(r), "");
  r.username = "";
  for (int i = 0; i < 32; i++)
    r.username += "\xD0\xB6";  // 32 Cyrillic letters: 64 bytes
  EXPECT_EQ(why(r), "");
  r.username += "\xD0\xB6";
  EXPECT_EQ(why(r), USERNAME_TOO_LONG);
}

TEST(MqttValidate, PortsOutsideTheRangeAreRefused) {
  for (int64_t port : {int64_t{0}, int64_t{-1}, int64_t{65536}, int64_t{70000}, INT64_MAX, INT64_MIN}) {
    MqttRecord r = valid();
    r.port = port;
    EXPECT_EQ(why(r), PORT_RANGE) << port;
  }
  for (int64_t port : {int64_t{1}, int64_t{1883}, int64_t{65535}}) {
    MqttRecord r = valid();
    r.port = port;
    EXPECT_EQ(why(r), "") << port;
  }
}

TEST(MqttValidate, BrokersByTheIpv4Rules) {
  for (const char *ok : {"broker", "broker.local", "192.168.1.10", "my_broker-1.lan"}) {
    MqttRecord r = valid();
    r.broker = ok;
    EXPECT_EQ(why(r, false), "") << ok;
  }
  const std::vector<std::pair<std::string, const char *>> refused = {
      {"fd00::10", BROKER_PORT},   {"host:1883", BROKER_PORT}, {"mqtts://host", BROKER_URL},
      {"host/path", BROKER_CHARS}, {"брокер", BROKER_CHARS},   {std::string("h\0st", 4), BROKER_CHARS},
      {" host", BROKER_CHARS},     {"host@lan", BROKER_CHARS},
  };
  for (const auto &[broker, message] : refused) {
    MqttRecord r = valid();
    r.broker = broker;
    EXPECT_EQ(why(r, false), message) << broker;
  }
}

// With IPv6 an address has two colons or more, so only a lone one reads as a port.
TEST(MqttValidate, BrokersByTheIpv6Rules) {
  for (const char *ok : {"fd00::10", "2001:db8::1", "::1", "broker.local"}) {
    MqttRecord r = valid();
    r.broker = ok;
    EXPECT_EQ(why(r, true), "") << ok;
  }
  MqttRecord r = valid();
  r.broker = "host:1883";
  EXPECT_EQ(why(r, true), BROKER_PORT);
  r.broker = "mqtt://[fd00::10]";
  EXPECT_EQ(why(r, true), BROKER_URL);
  r.broker = "[fd00::10]";
  EXPECT_EQ(why(r, true), BROKER_CHARS);
}

TEST(MqttValidate, ClientIdsArePrintableAsciiWithoutSpaces) {
  for (const char *ok : {"", "jxd-1", "!~#+/", "a"}) {
    MqttRecord r = valid();
    r.client_id = ok;
    EXPECT_EQ(why(r), "") << ok;
  }
  for (const char *bad : {"jxd 1", "jxd\t1", "ждх", "jxd\x7f"}) {
    MqttRecord r = valid();
    r.client_id = bad;
    EXPECT_EQ(why(r), CLIENT_ID_CHARS) << bad;
  }
}

TEST(MqttValidate, TopicPrefixes) {
  for (const char *ok : {"", "home", "home/jxd", "дом/реле", "a$b", "with space"}) {
    MqttRecord r = valid();
    r.topic_prefix = ok;
    EXPECT_EQ(why(r), "") << ok;
  }
  const std::vector<std::pair<std::string, const char *>> refused = {
      {"home/#", PREFIX_WILDCARD}, {"+", PREFIX_WILDCARD},   {"a\nb", PREFIX_TEXT},
      {"a\xC2\x85", PREFIX_TEXT},  {"a\xFF", PREFIX_TEXT},   {"$", PREFIX_DOLLAR},
      {"/", PREFIX_SLASH},         {"home//", PREFIX_SLASH}, {std::string("a\0b", 3), PREFIX_TEXT},
  };
  for (const auto &[prefix, message] : refused) {
    MqttRecord r = valid();
    r.topic_prefix = prefix;
    EXPECT_EQ(why(r), message) << prefix;
  }
}

TEST(MqttValidate, APasswordNeedsAUsernameAndMayHoldAnythingButNul) {
  MqttRecord r = valid();
  r.password = "p\tä w\x7f";
  EXPECT_EQ(why(r), "");
  r.username = "";
  r.password = "";
  EXPECT_EQ(why(r), "");
  r.password = "x";
  EXPECT_EQ(why(r), PASSWORD_NEEDS_USERNAME);
}

// Strict UTF-8 and no control characters: U+0000–U+001F and U+007F–U+009F.
TEST(MqttIsText, AcceptsTextAndRefusesEverythingElse) {
  for (const char *ok :
       {"", "plain", "кириллица", "€", "\xF0\x9F\x98\x80", "\xC2\xA0", "\xEF\xBF\xBD", "\xF4\x8F\xBF\xBF", "~"}) {
    EXPECT_TRUE(is_text(ok)) << ok;
  }
  const std::vector<std::string> refused = {
      std::string("\0", 1),  // NUL
      "\x01",
      "\x1F",
      "\t",
      "\n",
      "\x7F",
      "\xC2\x80",
      "\xC2\x9F",  // C1 controls
      "\x80",      // a stray continuation byte
      "\xC0\x80",
      "\xC1\xBF",  // overlong two-byte leads
      "\xE0\x80\x80",
      "\xE0\x9F\xBF",      // overlong three-byte forms
      "\xF0\x8F\xBF\xBF",  // an overlong four-byte form
      "\xED\xA0\x80",
      "\xED\xBF\xBF",      // surrogates
      "\xF4\x90\x80\x80",  // past U+10FFFF
      "\xF5\x80\x80\x80",
      "\xFF",          // leads that never start a sequence
      "\xE2\x82",      // cut short at the end
      "\xE2\x28\xA1",  // a continuation byte missing in the middle
      "\xD0",          // a lone lead
  };
  for (const std::string &bad : refused)
    EXPECT_FALSE(is_text(bad)) << ::testing::PrintToString(bad);
}

}  // namespace esphome::mqtt_config::testing
