#include "common.h"

namespace esphome::mqtt_config::testing {

// What a test delivers reaches the callbacks a device would call: the stand-in's matcher is
// upstream's, quirk included.
TEST(MqttStandIn, MatchesTopicsAsUpstreamDoes) {
  using mqtt::topic_match;
  EXPECT_TRUE(topic_match("sport/tennis", "sport/tennis"));
  EXPECT_FALSE(topic_match("sport/tennis", "sport/golf"));
  EXPECT_TRUE(topic_match("sport/tennis", "sport/+"));
  EXPECT_FALSE(topic_match("sport/tennis/player1", "sport/+"));
  EXPECT_TRUE(topic_match("sport/tennis", "sport/#"));
  EXPECT_TRUE(topic_match("sport/tennis/player1", "sport/#"));
  EXPECT_TRUE(topic_match("sport", "#"));
  // MQTT 3.1.1 counts zero levels under `#`; upstream's matcher does not, and neither does a
  // device, so a message on `sport` never reaches a `sport/#` subscription's callback.
  EXPECT_FALSE(topic_match("sport", "sport/#"));
  // No top-level wildcard reaches a `$` topic.
  EXPECT_FALSE(topic_match("$SYS/load", "#"));
  EXPECT_FALSE(topic_match("$SYS/load", "+/load"));
  EXPECT_TRUE(topic_match("$SYS/load", "$SYS/#"));
}

}  // namespace esphome::mqtt_config::testing
