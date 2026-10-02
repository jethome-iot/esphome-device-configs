#include "common.h"

namespace esphome::mqtt_subscriptions::testing {

static const std::vector<std::string> UNITS = {"°C", "%"};

static std::string check(const std::function<void(SlotConfig &)> &change, SlotKind kind = SlotKind::SENSOR) {
  SlotConfig slot = slot_of("Outdoor", "zigbee2mqtt/outdoor", kind);
  change(slot);
  return validate_slot(slot, UNITS);
}

TEST(SlotRules, AGoodSlotOfEachKindPasses) {
  for (SlotKind kind : {SlotKind::SENSOR, SlotKind::BINARY_SENSOR, SlotKind::TEXT_SENSOR}) {
    SCOPED_TRACE(kind_key(kind));
    EXPECT_EQ(check([](SlotConfig &) {}, kind), "");
  }
  EXPECT_EQ(check([](SlotConfig &s) {
              s.unit = "°C";
              s.decimals = 4;
              s.json_path = "a.0.b";
            }),
            "");
  // $SYS and the like are topics like any other.
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = "$SYS/broker/uptime"; }), "");
}

TEST(SlotRules, Name) {
  EXPECT_EQ(check([](SlotConfig &s) { s.name = ""; }), "'name' is required");
  EXPECT_EQ(check([](SlotConfig &s) { s.name = std::string(32, 'a'); }), "");
  EXPECT_EQ(check([](SlotConfig &s) { s.name = std::string(33, 'a'); }), "'name' is over 32 bytes");
  // Bytes, not characters: 17 Cyrillic letters are 34 bytes.
  EXPECT_EQ(check([](SlotConfig &s) {
              s.name.clear();
              for (int i = 0; i < 17; i++)
                s.name += "д";
            }),
            "'name' is over 32 bytes");
  for (const char *bad : {"a\nb", "a\x7f", "\xC3", "\xC0\x80", "\xED\xA0\x80"}) {
    SCOPED_TRACE(bad);
    EXPECT_EQ(check([bad](SlotConfig &s) { s.name = bad; }), "'name' must be text without control characters");
  }
  EXPECT_EQ(check([](SlotConfig &s) { s.name = "Температура"; }), "");
  // web_server would read it as a separator: two slots "A/Out" and "B/Out" would both be "Out".
  EXPECT_EQ(check([](SlotConfig &s) { s.name = "In/Out"; }), "'name' cannot contain '/'");
  EXPECT_EQ(check([](SlotConfig &s) { s.name = "/"; }), "'name' cannot contain '/'");
  EXPECT_EQ(check([](SlotConfig &s) { s.name = "a\n/b"; }), "'name' must be text without control characters");
}

TEST(SlotRules, Topic) {
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = ""; }), "'topic' is required");
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = std::string(128, 't'); }), "");
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = std::string(129, 't'); }), "'topic' is over 128 bytes");
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = std::string("a\0b", 3); }),
            "'topic' must be text without control characters");
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = "a\tb"; }), "'topic' must be text without control characters");
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = "\xFF"; }), "'topic' must be text without control characters");
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = " a"; }), "'topic' cannot start or end with a space");
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = "a "; }), "'topic' cannot start or end with a space");
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = "home/+/temp"; }),
            "'topic' cannot contain '+' or '#': a slot takes one topic");
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = "home/#"; }),
            "'topic' cannot contain '+' or '#': a slot takes one topic");
  EXPECT_EQ(check([](SlotConfig &s) { s.topic = "a b/c"; }), "");
}

TEST(SlotRules, JsonPath) {
  EXPECT_EQ(check([](SlotConfig &s) { s.json_path = std::string(64, 'k'); }), "");
  EXPECT_EQ(check([](SlotConfig &s) { s.json_path = std::string(65, 'k'); }), "'json_path' is over 64 bytes");
  EXPECT_EQ(check([](SlotConfig &s) { s.json_path = "a.b.c.d.e.f"; }), "");
  for (const char *bad : {"a.b.c.d.e.f.g", ".a", "a.", "a..b", "."}) {
    SCOPED_TRACE(bad);
    EXPECT_EQ(check([bad](SlotConfig &s) { s.json_path = bad; }), "'json_path' must be up to 6 keys separated by '.'");
  }
  EXPECT_FALSE(json_path_valid(""));
  // The path goes to the log and back to the dashboard, so it is text like the rest.
  for (const char *bad : {"a\nb", "a.\xFF", "\x7F"}) {
    SCOPED_TRACE(bad);
    EXPECT_EQ(check([bad](SlotConfig &s) { s.json_path = bad; }),
              "'json_path' must be text without control characters");
  }
  EXPECT_EQ(check([](SlotConfig &s) { s.json_path = "temperatura.значение"; }), "");
  EXPECT_EQ(check([](SlotConfig &s) { s.json_path = std::string(65, '\n'); }), "'json_path' is over 64 bytes");
}

TEST(SlotRules, UnitAndDecimalsBindOnlyANumber) {
  EXPECT_EQ(check([](SlotConfig &s) { s.unit = "%"; }), "");
  EXPECT_EQ(check([](SlotConfig &s) { s.unit = "K"; }), "'unit' is not one this firmware offers");
  EXPECT_EQ(check([](SlotConfig &s) { s.decimals = -1; }), "'decimals' must be a whole number from 0 to 4");
  EXPECT_EQ(check([](SlotConfig &s) { s.decimals = 5; }), "'decimals' must be a whole number from 0 to 4");
  EXPECT_EQ(check([](SlotConfig &s) { s.decimals = 0; }), "");
  EXPECT_EQ(check(
                [](SlotConfig &s) {
                  s.unit = "K";
                  s.decimals = 9;
                },
                SlotKind::TEXT_SENSOR),
            "");
}

TEST(SlotRules, PayloadsBindOnlyAnOnOff) {
  const auto binary = SlotKind::BINARY_SENSOR;
  EXPECT_EQ(check([](SlotConfig &s) { s.payload_on = ""; }, binary), "'payload_on' must be 1 to 32 bytes");
  EXPECT_EQ(check([](SlotConfig &s) { s.payload_on = std::string(33, 'x'); }, binary),
            "'payload_on' must be 1 to 32 bytes");
  EXPECT_EQ(check([](SlotConfig &s) { s.payload_off = ""; }, binary), "'payload_off' must be 1 to 32 bytes");
  EXPECT_EQ(check([](SlotConfig &s) { s.payload_off = std::string(33, 'x'); }, binary),
            "'payload_off' must be 1 to 32 bytes");
  EXPECT_EQ(check(
                [](SlotConfig &s) {
                  s.payload_on = "Open";
                  s.payload_off = "oPEN";
                },
                binary),
            "'payload_on' and 'payload_off' must differ");
  EXPECT_EQ(check([](SlotConfig &s) { s.payload_on = "a\tb"; }, binary),
            "'payload_on' must be text without control characters");
  EXPECT_EQ(check([](SlotConfig &s) { s.payload_off = "\xC3"; }, binary),
            "'payload_off' must be text without control characters");
  // Length first, then the text, then the next payload.
  EXPECT_EQ(check(
                [](SlotConfig &s) {
                  s.payload_on = "\n";
                  s.payload_off = "";
                },
                binary),
            "'payload_on' must be text without control characters");
  EXPECT_EQ(check([](SlotConfig &s) { s.payload_on = "Открыто"; }, binary), "");
  EXPECT_EQ(check([](SlotConfig &s) { s.payload_on = ""; }), "");
}

TEST(SlotRules, TheFirstRuleBrokenIsTheAnswer) {
  EXPECT_EQ(check([](SlotConfig &s) {
              s.name = "";
              s.topic = "";
              s.unit = "K";
            }),
            "'name' is required");
}

TEST(SlotRules, ObjectIdsAreWhatEntityBaseMakes) {
  EXPECT_EQ(object_id_of("Garage Door-2"), "garage_door-2");
  EXPECT_EQ(object_id_of("a.b/c"), "a_b_c");
  // Every byte outside [a-z0-9_-] is one '_', so equal byte lengths collide.
  EXPECT_EQ(object_id_of("Дверь"), object_id_of("Окно!!"));
  EXPECT_EQ(object_id_of("Дверь").size(), std::strlen("Дверь"));
}

// --- What another entity, another slot or a probe already has ---

class NamesTest : public SlotsTest {};

TEST_F(NamesTest, AnotherSlotsNameIsTaken) {
  this->plant({slot_of("Дверь", "a")});
  this->boot();
  EXPECT_EQ(this->post(R"({"slot":2,"enabled":true,"name":"Окно!!","topic":"b","kind":"text_sensor"})").message,
            "'name' gives the same id as slot 1; add a Latin letter or a digit");
  // Disabled counts too: enabling it later would be refused.
  this->plant({slot_of("A", "a"), [] {
                 SlotConfig s = slot_of("Outdoor", "b");
                 s.enabled = false;
                 return s;
               }()});
  EXPECT_EQ(this->post(R"({"slot":3,"enabled":true,"name":"outdoor","topic":"c","kind":"sensor"})").message,
            "'name' gives the same id as slot 2; add a Latin letter or a digit");
  EXPECT_EQ(this->post(R"({"slot":3,"enabled":true,"name":"Outdoor 2","topic":"c","kind":"sensor"})").message,
            "Slot 3 saved; applies after a reboot");
}

TEST_F(NamesTest, AnEntityOfTheSameKindIsTaken) {
  this->boot();
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"board temperature","topic":"a","kind":"sensor"})").message,
            "'name' gives the same id as the entity 'Board temperature'");
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"Input 1","topic":"a","kind":"binary_sensor"})").message,
            "'name' gives the same id as the entity 'Input 1'");
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"IP_address","topic":"a","kind":"text_sensor"})").message,
            "'name' gives the same id as the entity 'IP address'");
  // Another kind is another id space.
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"Input 1","topic":"a","kind":"sensor"})").message,
            "Slot 1 saved; applies after a reboot");
}

TEST_F(NamesTest, AnInternalEntityTakesNoName) {
  static sensor::Sensor *hidden = [] {
    auto *s = new sensor::Sensor();
    return s;
  }();
  this->boot();
  App.register_sensor(hidden, "Hidden", 0, 1u << ENTITY_FIELD_INTERNAL_SHIFT);
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"Hidden","topic":"a","kind":"sensor"})").message,
            "Slot 1 saved; applies after a reboot");
}

// The running entities are this component's own: what is saved decides, not what runs.
TEST_F(NamesTest, ARunningSlotDoesNotBlockItsOwnNameOrAnother) {
  this->plant({slot_of("Outdoor", "a")});
  this->boot();
  ASSERT_TRUE(this->subs->active(0));
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"Outdoor","topic":"b","kind":"sensor"})").message,
            "Slot 1 saved; applies after a reboot");
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"Indoor","topic":"b","kind":"sensor"})").message,
            "Slot 1 saved; applies after a reboot");
  // Slot 1's entity still runs as "Outdoor", but after the reboot that name is free.
  EXPECT_EQ(this->post(R"({"slot":2,"enabled":true,"name":"Outdoor","topic":"c","kind":"sensor"})").message,
            "Slot 2 saved; applies after a reboot");
}

TEST_F(NamesTest, TheSameHoldsForOnOffAndText) {
  this->plant({slot_of("Door", "a", SlotKind::BINARY_SENSOR), slot_of("Weather", "b", SlotKind::TEXT_SENSOR)});
  this->boot();
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"Door","topic":"c","kind":"binary_sensor"})").result,
            MqttSubscriptions::Result::OK);
  EXPECT_EQ(this->post(R"({"slot":2,"enabled":true,"name":"Weather","topic":"d","kind":"text_sensor"})").result,
            MqttSubscriptions::Result::OK);
}

TEST_F(NamesTest, ANameAProbeWillTakeIsRefusedOnSave) {
  this->boot([](TestSubscriptions &s) { s.reserve_sensor_names("Temp", 16); });
  JsonDocument doc = this->get();
  EXPECT_EQ(doc["reserved_names"]["prefix"], "Temp");
  EXPECT_EQ(doc["reserved_names"]["count"], 16);
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"Temp 3","topic":"a","kind":"sensor"})").message,
            "'name' gives the same id as 'Temp 3', which a temperature probe takes");
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"temp_16","topic":"a","kind":"sensor"})").message,
            "'name' gives the same id as 'Temp 16', which a temperature probe takes");
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"Temp 17","topic":"a","kind":"sensor"})").result,
            MqttSubscriptions::Result::OK);
  // Probes are sensors only.
  EXPECT_EQ(this->post(R"({"slot":2,"enabled":true,"name":"Temp 3","topic":"a","kind":"text_sensor"})").message,
            "Slot 2 saved; applies after a reboot");
}

// At boot no probe exists yet, so only the reservation keeps the slot from taking the name.
TEST_F(NamesTest, ANameAProbeWillTakeDoesNotRunAtBoot) {
  this->plant({slot_of("temp_3", "a"), slot_of("Temp 3", "b", SlotKind::TEXT_SENSOR)});
  TestSubscriptions &s = this->boot([](TestSubscriptions &s) { s.reserve_sensor_names("Temp", 16); });
  EXPECT_FALSE(s.active(0));
  EXPECT_EQ(s.state(0), SlotState::OFF);
  JsonDocument doc = this->get();
  EXPECT_EQ(doc["slots"][0]["status"]["error"],
            "invalid: 'name' gives the same id as 'Temp 3', which a temperature probe takes");
  EXPECT_TRUE(s.active(1));
}

// A hand-edited file: of two slots alike, the first keeps the name.
TEST_F(NamesTest, AtBootTheFirstOfTwoAlikeRuns) {
  this->plant({slot_of("Door", "a"), slot_of("door", "b", SlotKind::BINARY_SENSOR)});
  TestSubscriptions &s = this->boot();
  EXPECT_TRUE(s.active(0));
  EXPECT_FALSE(s.active(1));
  EXPECT_EQ(this->get()["slots"][1]["status"]["error"],
            "invalid: 'name' gives the same id as slot 1; add a Latin letter or a digit");
}

TEST_F(NamesTest, AtBootAnEntityOfTheSameKindKeepsItsName) {
  this->plant({slot_of("Input 1", "a", SlotKind::BINARY_SENSOR), slot_of("Input 1", "b")});
  TestSubscriptions &s = this->boot();
  EXPECT_FALSE(s.active(0));
  EXPECT_EQ(this->get()["slots"][0]["status"]["error"], "invalid: 'name' gives the same id as the entity 'Input 1'");
  EXPECT_TRUE(s.active(1));
}

}  // namespace esphome::mqtt_subscriptions::testing
