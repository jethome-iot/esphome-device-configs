#include "common.h"

namespace esphome::mqtt_subscriptions::testing {

// What the display's MQTT rows read: the running slots, by 0-based index.
class DisplayTest : public SlotsTest {};

TEST_F(DisplayTest, EachKindAsTheRowShowsIt) {
  SlotConfig outdoor = slot_of("Outdoor", "o");
  outdoor.unit = "°C";
  SlotConfig count = slot_of("Count", "c");
  count.decimals = 0;
  this->plant(
      {outdoor, slot_of("Door", "d", SlotKind::BINARY_SENSOR), slot_of("Weather", "w", SlotKind::TEXT_SENSOR), count});
  TestSubscriptions &s = this->boot();
  EXPECT_EQ(s.max_slots(), 4u);
  for (size_t i = 0; i < 4; i++) {
    SCOPED_TRACE(i);
    EXPECT_TRUE(s.active(i));
    EXPECT_FALSE(s.has_value(i));
    EXPECT_EQ(s.value_text(i), "");
  }
  EXPECT_EQ(s.name(0), "Outdoor");
  EXPECT_EQ(s.name(2), "Weather");

  this->online();
  this->deliver("o", "21.46");
  this->deliver("d", "ON");
  this->deliver("w", "Sunny");
  this->deliver("c", "7.6");
  EXPECT_TRUE(s.has_value(0));
  EXPECT_EQ(s.value_text(0), "21.5 °C");
  EXPECT_EQ(s.value_text(1), "On");
  EXPECT_EQ(s.value_text(2), "Sunny");
  EXPECT_EQ(s.value_text(3), "8");
  this->deliver("d", "OFF");
  EXPECT_EQ(s.value_text(1), "Off");

  // A message arrived, but the value it carried is unknown.
  this->deliver("o", "unavailable");
  this->deliver("d", "maybe");
  EXPECT_TRUE(s.has_value(0));
  EXPECT_EQ(s.value_text(0), "");
  EXPECT_EQ(s.value_text(1), "");
}

TEST_F(DisplayTest, SlotsThatDoNotRunAreEmpty) {
  SlotConfig disabled = slot_of("Spare", "s");
  disabled.enabled = false;
  this->plant({disabled});
  TestSubscriptions &s = this->boot();
  for (size_t slot : {size_t{0}, size_t{3}, size_t{4}, size_t{1000}}) {
    SCOPED_TRACE(slot);
    EXPECT_FALSE(s.active(slot));
    EXPECT_EQ(s.name(slot), "");
    EXPECT_FALSE(s.has_value(slot));
    EXPECT_EQ(s.value_text(slot), "");
    EXPECT_EQ(s.state(slot), SlotState::OFF);
  }
}

TEST_F(DisplayTest, StateKeys) {
  EXPECT_STREQ(MqttSubscriptions::state_key(SlotState::OFF), "off");
  EXPECT_STREQ(MqttSubscriptions::state_key(SlotState::WAITING), "waiting");
  EXPECT_STREQ(MqttSubscriptions::state_key(SlotState::OK), "ok");
  EXPECT_STREQ(MqttSubscriptions::state_key(SlotState::ERROR), "error");
  EXPECT_STREQ(MqttSubscriptions::state_key(SlotState::SUSPENDED), "suspended");
  EXPECT_STREQ(kind_key(SlotKind::SENSOR), "sensor");
  EXPECT_STREQ(kind_key(SlotKind::BINARY_SENSOR), "binary_sensor");
  EXPECT_STREQ(kind_key(SlotKind::TEXT_SENSOR), "text_sensor");
}

}  // namespace esphome::mqtt_subscriptions::testing
