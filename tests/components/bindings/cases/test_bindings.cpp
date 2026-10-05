#include "common.h"

namespace esphome::bindings::testing {

TEST(BindingMode, NamesRoundTripAndUnknownOnesAreRefused) {
  BindingMode mode = BindingMode::TOGGLE;
  EXPECT_TRUE(parse_binding_mode("none", mode));
  EXPECT_EQ(mode, BindingMode::NONE);
  EXPECT_TRUE(parse_binding_mode("", mode));  // an empty record field
  EXPECT_EQ(mode, BindingMode::NONE);
  EXPECT_TRUE(parse_binding_mode("toggle", mode));
  EXPECT_EQ(mode, BindingMode::TOGGLE);
  EXPECT_TRUE(parse_binding_mode("follow", mode));
  EXPECT_EQ(mode, BindingMode::FOLLOW);
  EXPECT_FALSE(parse_binding_mode("Toggle", mode));
  EXPECT_EQ(mode, BindingMode::FOLLOW);  // untouched
  EXPECT_FALSE(parse_binding_mode(nullptr, mode));
  for (BindingMode m : {BindingMode::NONE, BindingMode::TOGGLE, BindingMode::FOLLOW}) {
    BindingMode back = BindingMode::NONE;
    EXPECT_TRUE(parse_binding_mode(binding_mode_to_string(m), back));
    EXPECT_EQ(back, m);
  }
}

TEST_F(Bindings, ToggleFlipsOnTheRisingEdgeOnly) {
  manager->setup();
  manager->set_binding(RELAY_1, IN_1, BindingMode::TOGGLE);
  e.in1.publish_state(true);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 1);
  e.in1.publish_state(false);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 1);
  press(e.in1);
  EXPECT_FALSE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 2);
  EXPECT_EQ(e.relay2.writes, 0);
}

TEST_F(Bindings, FollowMirrorsTheInput) {
  manager->setup();
  e.in1.publish_state(true);
  // Binding an input that already has a state drives the output at once.
  manager->set_binding(RELAY_1, IN_1, BindingMode::FOLLOW);
  EXPECT_TRUE(e.relay1.state);
  e.in1.publish_state(false);
  EXPECT_FALSE(e.relay1.state);
  e.in1.publish_state(true);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 3);
}

TEST_F(Bindings, TheSameBindingAgainDrivesNothing) {
  manager->setup();
  e.in1.publish_state(true);
  manager->set_binding(RELAY_1, IN_1, BindingMode::FOLLOW);
  EXPECT_EQ(e.relay1.writes, 1);
  manager->set_binding(RELAY_1, IN_1, BindingMode::FOLLOW);  // every edit of the relay's record repeats it
  EXPECT_EQ(e.relay1.writes, 1);
  manager->set_binding(RELAY_1, IN_2, BindingMode::FOLLOW);  // a changed one drives again
  EXPECT_EQ(e.relay1.writes, 2);
  EXPECT_FALSE(e.relay1.state);
}

TEST_F(Bindings, OneInputMayDriveSeveralOutputsAndAnOutputHasOneBinding) {
  manager->setup();
  manager->set_binding(RELAY_1, IN_1, BindingMode::TOGGLE);
  manager->set_binding(RELAY_2, IN_1, BindingMode::FOLLOW);
  e.in1.publish_state(true);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_TRUE(e.relay2.state);

  // Rebinding replaces: relay 1 now follows input 2, copying its level at once, and ignores
  // input 1.
  manager->set_binding(RELAY_1, IN_2, BindingMode::FOLLOW);
  EXPECT_FALSE(e.relay1.state);
  press(e.in1);
  EXPECT_FALSE(e.relay1.state);
  EXPECT_FALSE(e.relay2.state);  // still following input 1, which the press left low
  e.in2.publish_state(true);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_FALSE(e.relay2.state);
}

TEST_F(Bindings, NoneOrNoInputUnbinds) {
  manager->setup();
  manager->set_binding(RELAY_1, IN_1, BindingMode::TOGGLE);
  manager->set_binding(RELAY_1, IN_1, BindingMode::NONE);
  press(e.in1);
  EXPECT_EQ(e.relay1.writes, 0);
  manager->set_binding(RELAY_1, IN_1, BindingMode::TOGGLE);
  manager->set_binding(RELAY_1, 0, BindingMode::TOGGLE);
  press(e.in1);
  EXPECT_EQ(e.relay1.writes, 0);
  EXPECT_FALSE(manager->remove_binding(RELAY_1));
}

TEST_F(Bindings, BootLevelsDriveFollowOutputsOnceAndNeverToggle) {
  // Before setup(): the inputs' boot levels arrive, nothing may move yet.
  e.in1.publish_state(true);
  e.in2.publish_state(true);
  manager->set_binding(RELAY_1, IN_1, BindingMode::TOGGLE);
  manager->set_binding(RELAY_2, IN_2, BindingMode::FOLLOW);
  e.in1.publish_state(false);
  e.in1.publish_state(true);
  EXPECT_EQ(e.relay1.writes, 0);
  EXPECT_EQ(e.relay2.writes, 0);

  manager->setup();
  EXPECT_EQ(e.relay1.writes, 0);  // a toggle needs an edge, and a level is not one
  EXPECT_TRUE(e.relay2.state);    // follow copies the level it finds
  EXPECT_EQ(e.relay2.writes, 1);
}

TEST_F(Bindings, AMissingInputIsKeptAndLogged) {
  manager->setup();
  manager->set_binding(RELAY_1, NO_SUCH, BindingMode::TOGGLE);
  EXPECT_TRUE(log().has("not found for output"));
  // Still on the table: unbinding it reports a removal.
  EXPECT_TRUE(manager->remove_binding(RELAY_1));
}

TEST_F(Bindings, AnAnnouncedLevelIsNotAnEdge) {
  manager->setup();
  manager->set_binding(RELAY_1, IN_1, BindingMode::TOGGLE);
  manager->set_binding(RELAY_2, IN_1, BindingMode::FOLLOW);

  // An inversion flip re-emits the input's state: the follow output copies it, the toggle
  // output stays.
  manager->expect_level(IN_1);
  e.in1.publish_state(true);
  EXPECT_FALSE(e.relay1.state);
  EXPECT_TRUE(e.relay2.state);

  // The mark is spent: the next real edge toggles again.
  e.in1.publish_state(false);
  e.in1.publish_state(true);
  EXPECT_TRUE(e.relay1.state);

  // An input nobody listens to takes no mark, so a later binding sees its first edge.
  manager->expect_level(IN_2);
  manager->set_binding(RELAY_1, IN_2, BindingMode::TOGGLE);
  e.in2.publish_state(true);
  EXPECT_FALSE(e.relay1.state);
}

}  // namespace esphome::bindings::testing

namespace esphome::bindings::testing {

// A running thermostat's relay is the thermostat's: no binding moves it, and the log says who has it.
TEST_F(Bindings, AHeldOutputIsLeftAlone) {
  manager->setup();
  holder.held[&e.relay1] = "Living room";
  manager->set_binding(RELAY_1, IN_1, BindingMode::FOLLOW);
  manager->set_binding(RELAY_2, IN_2, BindingMode::TOGGLE);
  e.in1.publish_state(true);
  e.in1.publish_state(false);
  EXPECT_EQ(e.relay1.writes, 0);
  EXPECT_TRUE(log().has_info("'Relay 1' left alone: thermostat 'Living room' drives it"));

  holder.held[&e.relay2] = "Floor";
  press(e.in2);
  EXPECT_EQ(e.relay2.writes, 0);
  holder.held.erase(&e.relay2);
  press(e.in2);
  EXPECT_EQ(e.relay2.writes, 1) << "free again: the next edge toggles";
}

TEST_F(Bindings, TheBootDriveAndANewBindingSkipAHeldOutput) {
  e.in1.publish_state(true);
  e.in2.publish_state(true);
  manager->set_binding(RELAY_1, IN_1, BindingMode::FOLLOW);
  holder.held[&e.relay1] = "Living room";
  manager->setup();
  EXPECT_EQ(e.relay1.writes, 0);

  holder.held[&e.relay2] = "Floor";
  manager->set_binding(RELAY_2, IN_2, BindingMode::FOLLOW);
  EXPECT_EQ(e.relay2.writes, 0);
}

// Once the thermostat lets go, a follow output takes the input's level at once; a toggle one
// waits for the next edge, and an output nothing binds stays where the thermostat left it.
TEST_F(Bindings, AFollowBindingTakesItsOutputBackOnRelease) {
  manager->setup();
  holder.held[&e.relay1] = "Living room";
  holder.held[&e.relay2] = "Floor";
  manager->set_binding(RELAY_1, IN_1, BindingMode::FOLLOW);
  manager->set_binding(RELAY_2, IN_2, BindingMode::TOGGLE);
  e.in1.publish_state(true);
  e.in2.publish_state(true);

  holder.release(&e.relay1);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay1.writes, 1);
  holder.release(&e.relay2);
  EXPECT_EQ(e.relay2.writes, 0);

  manager->remove_binding(RELAY_1);
  holder.held[&e.relay1] = "Living room";
  holder.release(&e.relay1);
  EXPECT_EQ(e.relay1.writes, 1);
}

TEST_F(Bindings, AReleaseWhileTheInputHasNoStateDrivesNothing) {
  manager->setup();
  manager->set_binding(RELAY_1, IN_1, BindingMode::FOLLOW);
  EXPECT_EQ(e.relay1.writes, 1);
  e.in1.invalidate_state();
  holder.held[&e.relay1] = "Living room";
  holder.release(&e.relay1);
  EXPECT_EQ(e.relay1.writes, 1);
}

// An output this build lacks is kept like a missing input, and only logged when driven.
TEST_F(Bindings, AMissingOutputIsLoggedWhenDriven) {
  manager->setup();
  manager->set_binding(NO_SUCH, IN_1, BindingMode::FOLLOW);
  e.in1.publish_state(true);
  EXPECT_TRUE(log().has("Output 0x"));
  EXPECT_TRUE(manager->remove_binding(NO_SUCH));
}

// After the thermostats have claimed their relays (climate_hub, DATA - 0.5), before the rules
// (automations, DATA - 1).
TEST_F(Bindings, SetsUpBetweenTheThermostatsAndTheRules) {
  EXPECT_LT(manager->get_setup_priority(), setup_priority::DATA - 0.5f);
  EXPECT_GT(manager->get_setup_priority(), setup_priority::DATA - 1.f);
}

}  // namespace esphome::bindings::testing
