#include "common.h"
#include "esphome/components/mqtt/mqtt_const.h"
#include "esphome/components/switch_hold/switch_hold.h"

namespace esphome::mqtt_config::testing {

// A thermostat stand-in: holds one switch, under one name.
class OneHolder : public switch_hold::SwitchHolder {
 public:
  std::string holder_of(const switch_::Switch *sw) const override {
    return sw != nullptr && sw == this->held ? "Hall" : "";
  }
  const switch_::Switch *held{nullptr};
};

// Relay 1's commands go through a HoldAwareSwitch, as codegen wires every visible switch.
class HoldTest : public MqttTest {
 protected:
  void SetUp() override {
    MqttTest::SetUp();
    switch_hold::set_holder(&this->holder);
    entities().relay1.turn_off();
  }
  void TearDown() override {
    switch_hold::set_holder(nullptr);
    MqttTest::TearDown();
  }

  // Left allocated, as the config is: its state callback stays on the process-wide switch.
  TestConfig &boot_held() {
    Entities &e = entities();
    this->held = new HoldAwareSwitch(&e.relay1);  // NOLINT(cppcoreguidelines-owning-memory)
    TestConfig &c =
        this->boot([this, &e](TestConfig &config) { config.hold_switch(this->relay1.get(), this->held, &e.relay1); });
    this->held->call_setup();
    this->client->connect_for_test();
    this->settle();
    this->client->published.clear();
    LogCapture::instance().clear();
    return c;
  }
  void command(const char *payload) {
    this->client->deliver_for_test(std::string(NODE_PREFIX) + "/switch/relay_1/command", payload);
  }
  std::vector<std::string> states_published() const {
    std::vector<std::string> out;
    for (const auto &p : this->client->published) {
      if (p.topic == std::string(NODE_PREFIX) + "/switch/relay_1/state")
        out.push_back(p.payload);
    }
    return out;
  }

  OneHolder holder;
  HoldAwareSwitch *held{nullptr};
};

TEST_F(HoldTest, UpstreamsComponentGoesQuietAndTheHeldOneTakesItsTopics) {
  TestConfig &c = this->boot_held();
  EXPECT_TRUE(this->relay1->is_internal());
  EXPECT_FALSE(this->held->is_internal());
  const auto &children = this->client->children();
  EXPECT_EQ(std::count(children.begin(), children.end(), this->held), 1);
  EXPECT_EQ(std::count(children.begin(), children.end(), this->relay1.get()), 0);
  const std::string topic = std::string(NODE_PREFIX) + "/switch/relay_1/command";
  const auto &subs = this->client->subscriptions();
  EXPECT_EQ(std::count_if(subs.begin(), subs.end(), [&topic](const auto &s) { return s.topic == topic; }), 1);
  // Walked for the discovery cleanup, next to the four codegen gave.
  EXPECT_EQ(c.walked(), 5u);
}

TEST_F(HoldTest, ACommandMovesASwitchNobodyHolds) {
  this->boot_held();
  this->command("ON");
  EXPECT_TRUE(entities().relay1.state);
  this->command("toggle");
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_FALSE(LogCapture::instance().has("refused"));
}

TEST_F(HoldTest, ACommandThatWouldMoveAHeldSwitchIsRefused) {
  this->boot_held();
  this->holder.held = &entities().relay1;
  this->command("ON");
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_TRUE(LogCapture::instance().has("'Relay 1' is driven by thermostat 'Hall'; the MQTT command is refused"));
  // A client that assumed the change hears the state the switch kept.
  EXPECT_EQ(this->states_published(), std::vector<std::string>{"OFF"});
}

TEST_F(HoldTest, AToggleOfAHeldSwitchIsRefused) {
  this->boot_held();
  entities().relay1.turn_on();
  this->holder.held = &entities().relay1;
  this->client->published.clear();
  this->command("TOGGLE");
  EXPECT_TRUE(entities().relay1.state);
  EXPECT_EQ(this->states_published(), std::vector<std::string>{"ON"});
}

// As a Modbus write of the state a held relay already has: nothing moves, so nothing is refused.
TEST_F(HoldTest, TheStateAHeldSwitchAlreadyHasIsTaken) {
  this->boot_held();
  this->holder.held = &entities().relay1;
  this->command("OFF");
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_FALSE(LogCapture::instance().has("refused"));
}

TEST_F(HoldTest, AnotherSwitchHeldLeavesThisOneFree) {
  this->boot_held();
  this->holder.held = &entities().relay2;
  this->command("ON");
  EXPECT_TRUE(entities().relay1.state);
}

TEST_F(HoldTest, AnUnknownPayloadChangesNothing) {
  this->boot_held();
  this->command("maybe");
  EXPECT_FALSE(entities().relay1.state);
  EXPECT_TRUE(LogCapture::instance().has("'Relay 1': Received unknown status payload: maybe"));
  EXPECT_TRUE(this->states_published().empty());
}

// Upstream's discovery for a switch: optimistic when the switch only assumes its state.
TEST(HoldAwareSwitchDiscovery, AnAssumedStateSwitchIsOptimistic) {
  class AssumedSwitch : public FakeSwitch {
   public:
    bool assumed_state() override { return true; }
  };
  AssumedSwitch assumed;
  FakeSwitch known;
  mqtt::SendDiscoveryConfig config;
  JsonDocument doc;
  HoldAwareSwitch(&assumed).send_discovery(doc.to<JsonObject>(), config);
  EXPECT_TRUE(doc[mqtt::MQTT_OPTIMISTIC].as<bool>());
  doc.clear();
  HoldAwareSwitch(&known).send_discovery(doc.to<JsonObject>(), config);
  EXPECT_TRUE(doc[mqtt::MQTT_OPTIMISTIC].isUnbound());
}

TEST_F(HoldTest, TheHeldSwitchIsAnnouncedAndCleanedInPlaceOfUpstreams) {
  MqttRecord stored = enabled_record();
  stored.discovery = true;
  this->plant(stored);
  Entities &e = entities();
  this->held = new HoldAwareSwitch(&e.relay1);  // NOLINT(cppcoreguidelines-owning-memory)
  this->boot([this, &e](TestConfig &c) { c.hold_switch(this->relay1.get(), this->held, &e.relay1); });
  this->held->call_setup();
  this->connect_and_settle();
  // Relay 1 (the held one), Relay 2 and Input 1; Hidden stays off.
  EXPECT_EQ(this->discovery_publishes("{}"), 3u);
  this->client->published.clear();
  this->save(patch_of([](MqttPatch &p) { p.discovery = false; }));
  EXPECT_TRUE(this->held->is_resend_pending());
  this->settle();
  EXPECT_EQ(this->discovery_publishes(""), 3u);
}

}  // namespace esphome::mqtt_config::testing
