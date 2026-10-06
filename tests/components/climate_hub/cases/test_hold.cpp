#include "common.h"
#include "esphome/components/switch_hold/switch_hold.h"

namespace esphome::climate_hub::testing {

// What switch_hold heard, each release with who held the relay at that moment. Registered once:
// the registry keeps its listeners for the life of the process.
struct Release {
  switch_::Switch *relay;
  std::string holder_then;
};

inline std::vector<Release> &releases() {
  static std::vector<Release> *heard = [] {
    auto *list = new std::vector<Release>();
    switch_hold::add_on_release_callback([list](switch_::Switch *sw) {
      list->push_back({sw, switch_hold::holder(sw)});
    });
    return list;
  }();
  return *heard;
}

// A Follow binding whose input is on, on every relay: what bindings does on a release, closing
// the relay unless a thermostat holds it. Off until a case turns it on.
struct Follow {
  bool on{false};
  std::vector<switch_::Switch *> moved;
};

inline Follow &follow() {
  static Follow *binding = [] {
    auto *f = new Follow();
    switch_hold::add_on_release_callback([f](switch_::Switch *sw) {
      if (!f->on || !switch_hold::holder(sw).empty())
        return;
      sw->turn_on();
      f->moved.push_back(sw);
    });
    return f;
  }();
  return *binding;
}

class HoldTest : public HubTest {
 protected:
  void SetUp() override {
    HubTest::SetUp();
    releases().clear();
    follow().on = false;
    follow().moved.clear();
  }
  // The listener outlives the case: left on, it would close relays under the other suites.
  void TearDown() override {
    follow().on = false;
    HubTest::TearDown();
  }
  Entities &e = entities();
};

TEST_F(HoldTest, TheHubIsTheSwitchHolder) {
  this->create(draft("Living room", "relay_1"));
  EXPECT_EQ("Living room", switch_hold::holder(&e.relay1)) << "by its name, not its id";
  EXPECT_EQ("", switch_hold::holder(&e.relay2));
}

TEST_F(HoldTest, OnlyARunningThermostatHoldsItsRelays) {
  ClimateConfig waiting = draft("Waiting", "relay_1");
  waiting.sensor_id = "temp_9";  // not on the device: saved, waits
  ASSERT_TRUE(hub().create(waiting).ok);
  ClimateConfig disabled = draft("Disabled", "relay_2");
  disabled.enabled = false;
  this->create(disabled);
  EXPECT_FALSE(hub().is_running("waiting"));
  EXPECT_EQ("", switch_hold::holder(&e.relay1));
  EXPECT_EQ("", switch_hold::holder(&e.relay2));

  this->create(draft("Boiler", "relay_3"));
  EXPECT_EQ("Boiler", switch_hold::holder(&e.relay3));
  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  EXPECT_EQ("", switch_hold::holder(&e.relay3)) << "stopped";
}

TEST_F(HoldTest, ASaveMovesTheHoldWithTheRelayAndTheName) {
  this->create(draft("Boiler", "relay_1"));
  ClimateConfig moved = draft("Floor heating", "relay_2");
  ASSERT_TRUE(hub().update("boiler", moved).ok);
  EXPECT_EQ("", switch_hold::holder(&e.relay1));
  EXPECT_EQ("Floor heating", switch_hold::holder(&e.relay2));
}

TEST_F(HoldTest, ATakeOverHandsTheHoldToTheTaker) {
  this->create(draft("Winter", "relay_1"));
  ClimateConfig summer = draft("Summer", "relay_1");
  summer.enabled = false;
  this->create(summer);
  ASSERT_TRUE(hub().set_enabled("summer", true, true).ok);
  EXPECT_EQ("Summer", switch_hold::holder(&e.relay1));
  EXPECT_TRUE(releases().empty()) << "a relay that changes hands is never free";
}

TEST_F(HoldTest, StoppingOrRemovingAThermostatAnnouncesItsRelaysOnceFree) {
  ClimateConfig both = draft("Both", "relay_1");
  both.cool.relay_id = "relay_2";
  both.mode = HubMode::HEAT_COOL;
  this->create(both);
  ASSERT_TRUE(hub().set_enabled("both", false).ok);
  ASSERT_EQ(2u, releases().size());
  for (const Release &release : releases())
    EXPECT_EQ("", release.holder_then) << "heard only once nothing holds it";
  std::set<switch_::Switch *> relays{releases()[0].relay, releases()[1].relay};
  EXPECT_EQ((std::set<switch_::Switch *>{&e.relay1, &e.relay2}), relays);

  releases().clear();
  ASSERT_TRUE(hub().set_enabled("both", false).ok);
  EXPECT_TRUE(releases().empty()) << "nothing was held";

  ASSERT_TRUE(hub().set_enabled("both", true).ok);
  releases().clear();
  ASSERT_TRUE(hub().remove("both").ok);
  EXPECT_EQ(2u, releases().size());
}

TEST_F(HoldTest, ASaveThatDisablesAThermostatAnnouncesItsRelays) {
  ClimateConfig both = draft("Both", "relay_1");
  both.cool.relay_id = "relay_2";
  both.mode = HubMode::HEAT_COOL;
  this->create(both);
  both.enabled = false;
  ASSERT_TRUE(hub().update("both", both).ok);
  EXPECT_FALSE(hub().is_running("both"));
  ASSERT_EQ(2u, releases().size());
  for (const Release &release : releases())
    EXPECT_EQ("", release.holder_then);
  std::set<switch_::Switch *> relays{releases()[0].relay, releases()[1].relay};
  EXPECT_EQ((std::set<switch_::Switch *>{&e.relay1, &e.relay2}), relays);
}

TEST_F(HoldTest, ASaveThatDropsARelayAnnouncesThatOneOnly) {
  ClimateConfig both = draft("Both", "relay_1");
  both.cool.relay_id = "relay_2";
  both.mode = HubMode::HEAT_COOL;
  this->create(both);
  ASSERT_TRUE(hub().update("both", draft("Both", "relay_1")).ok);
  ASSERT_EQ(1u, releases().size());
  EXPECT_EQ(&e.relay2, releases()[0].relay);
  EXPECT_EQ("Both", switch_hold::holder(&e.relay1));
}

// The holder's other relay is free once the taker runs; the one the taker took is not.
TEST_F(HoldTest, ATakeOverAnnouncesOnlyWhatTheTakerLeft) {
  ClimateConfig winter = draft("Winter", "relay_1");
  winter.cool.relay_id = "relay_2";
  winter.mode = HubMode::HEAT_COOL;
  this->create(winter);
  ClimateConfig summer = draft("Summer", "relay_1");
  summer.enabled = false;
  this->create(summer);
  ASSERT_TRUE(hub().set_enabled("summer", true, true).ok);
  ASSERT_EQ(1u, releases().size());
  EXPECT_EQ(&e.relay2, releases()[0].relay);
}

// Nothing hears of a relay a thermostat keeps through a Save, or one a failed Save never got.
TEST_F(HoldTest, ARefusedOrKeepingSaveAnnouncesNothing) {
  this->create(draft("Boiler", "relay_1"));
  ClimateConfig renamed = draft("Big boiler", "relay_1");
  ASSERT_TRUE(hub().update("boiler", renamed).ok);
  EXPECT_EQ("Big boiler", switch_hold::holder(&e.relay1));

  this->create(draft("Other", "relay_2"));
  Result refused = hub().update("other", draft("Other", "relay_1"));
  EXPECT_EQ(409, refused.code);
  EXPECT_TRUE(releases().empty());
  EXPECT_EQ("Other", switch_hold::holder(&e.relay2));
}

// A thermostat that waited for a relay starts on it before anything hears of the release, so a
// Follow binding never closes it under that one; the stopped one's other relay is free.
TEST_F(HoldTest, AWaiterThatStartsOnAFreedRelayIsNotAnnounced) {
  write_file(this->file_of("summer"), file_doc("summer", "Summer", "relay_1", "relay_2"));
  write_file(this->file_of("winter"), file_doc("winter", "Winter", "relay_1"));
  this->reboot();
  ASSERT_TRUE(hub().is_running("summer"));
  ASSERT_FALSE(hub().is_running("winter"));
  releases().clear();
  follow().on = true;

  Result result = hub().set_enabled("summer", false);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(std::vector<std::string>{"winter"}, result.started);
  EXPECT_EQ("Winter", switch_hold::holder(&e.relay1));
  ASSERT_EQ(1u, releases().size());
  EXPECT_EQ(&e.relay2, releases()[0].relay);
  EXPECT_EQ(std::vector<switch_::Switch *>{&e.relay2}, follow().moved);
  EXPECT_FALSE(e.relay1.state);
  EXPECT_TRUE(e.relay2.state);
}

// The same after a take-over: the relay the holder drove alone goes to the one waiting for it.
TEST_F(HoldTest, ATakeOverAnnouncesNoRelayAWaiterStartsOn) {
  write_file(this->file_of("a"), file_doc("a", "A", "relay_1", "relay_2"));
  write_file(this->file_of("b"), file_doc("b", "B", "relay_2"));
  write_file(this->file_of("c"), file_doc("c", "C", "relay_1", "", false));
  this->reboot();
  ASSERT_TRUE(hub().is_running("a"));
  ASSERT_FALSE(hub().is_running("b"));
  releases().clear();
  follow().on = true;

  Result result = hub().set_enabled("c", true, true);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(std::vector<std::string>{"a"}, result.stopped);
  EXPECT_EQ(std::vector<std::string>{"b"}, result.started);
  EXPECT_EQ("C", switch_hold::holder(&e.relay1));
  EXPECT_EQ("B", switch_hold::holder(&e.relay2));
  EXPECT_TRUE(releases().empty());
  EXPECT_TRUE(follow().moved.empty());
}

}  // namespace esphome::climate_hub::testing
