// A mode's list: its actions in order, a wait that holds the rest, a new point that stops a list
// in a wait, everything that runs the list of the mode in force again, and a relay a thermostat
// holds, skipped and set once it is free.
#include "common.h"

namespace esphome::schedules::testing {

class Lists : public EngineTest {
 protected:
  void SetUp() override {
    EngineTest::SetUp();
    // Every write of every relay, in the order they came.
    const std::pair<FakeSwitch *, std::string> relays[] = {
        {&this->e.relay1, "r1"}, {&this->e.relay2, "r2"}, {&this->e.boiler, "boiler"}};
    for (const auto &relay : relays) {
      FakeSwitch *sw = relay.first;
      const std::string name = relay.second;
      sw->on_write = [this, sw, name]() { this->order.push_back(name + (sw->state ? " on" : " off")); };
    }
  }
  // `config` created while the clock is not valid; the first valid tick reads `when`.
  void start(int64_t when, const ScheduleConfig &config) {
    this->boot();
    this->create(config);
    this->engine->local = when;
    this->engine->tick();
  }
  // Mode A from 08:00: relay_1 on, a wait of `wait` seconds, relay_2 on. Mode B from `b_at`:
  // relay_1 off, relay_2 off.
  static ScheduleConfig two_modes(uint32_t wait = 600, uint16_t b_at = wm(0, 8, 5)) {
    ScheduleConfig c;
    c.name = "Lights";
    c.add_mode("a", "A");
    c.add_relay("relay_1", true);
    c.add_wait(wait);
    c.add_relay("relay_2", true);
    c.add_mode("b", "B");
    c.add_relay("relay_1", false);
    c.add_relay("relay_2", false);
    c.add_point(wm(0, 8, 0), 0);
    c.add_point(b_at, 1);
    return c;
  }

  std::vector<std::string> order;
};

TEST_F(Lists, TheActionsRunInTheListsOrder) {
  ScheduleConfig c;
  c.name = "Order";
  c.add_mode("go", "Go");
  c.add_relay("relay_2", true);
  c.add_relay("relay_1", true);
  c.add_relay("relay_2", false);
  c.add_point(0, 0);
  this->start(at_local(0, 9, 0), c);
  EXPECT_EQ((std::vector<std::string>{"r2 on", "r1 on", "r2 off"}), this->order);
  EXPECT_EQ(3, this->engine->list_next("order")) << "done";
  EXPECT_FALSE(this->engine->waiting("order"));
}

// The spec's example without its thermostat: the actions after the wait come two minutes on.
TEST_F(Lists, AWaitHoldsTheRestOfTheListUntilItIsOver) {
  this->start(at_local(0, 6, 29, 30), home());
  EXPECT_EQ((std::vector<std::string>{"r2 off", "r1 off"}), this->order) << "Night, in force since Sunday";
  this->order.clear();
  this->engine->run(30);
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order) << "06:30, Day";
  EXPECT_TRUE(this->engine->waiting("home"));
  EXPECT_EQ(1u, this->engine->timers.size());
  this->engine->run(119);
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order);
  this->engine->run(1);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "r2 on"}), this->order);
  EXPECT_FALSE(this->engine->waiting("home"));
  EXPECT_EQ(4, this->engine->list_next("home"));
  this->engine->run(3600);
  EXPECT_EQ(2u, this->order.size()) << "nothing more until 23:00";
}

TEST_F(Lists, ANewPointStopsAListInAWait) {
  this->start(at_local(0, 7, 59, 30), two_modes());
  this->order.clear();
  this->engine->run(30);
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order);
  this->engine->run(5 * 60);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "r1 off", "r2 off"}), this->order) << "B at 08:05, at once";
  EXPECT_TRUE(this->engine->timers.empty());
  this->engine->run(3600);
  EXPECT_EQ(3u, this->order.size()) << "A's relay_2 never comes";
}

// A point of the mode that runs starts its list over, from the first action, the wait too.
TEST_F(Lists, APointOfTheSameModeStartsItsListOver) {
  ScheduleConfig c = two_modes(120);
  c.points.clear();
  c.add_point(wm(0, 8, 0), 0);
  c.add_point(wm(0, 8, 1), 0);
  this->start(at_local(0, 7, 59, 30), c);
  this->order.clear();
  this->engine->run(30 + 60);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "r1 on"}), this->order) << "even with the relay on already";
  this->engine->run(119);
  EXPECT_EQ(2u, this->order.size()) << "the wait counts from 08:01";
  this->engine->run(1);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "r1 on", "r2 on"}), this->order);
}

TEST_F(Lists, EachScheduleWaitsOnItsOwn) {
  auto blink = [](const char *name, const char *relay, uint32_t wait) {
    ScheduleConfig c;
    c.name = name;
    c.add_mode("blink", "Blink");
    c.add_relay(relay, true);
    c.add_wait(wait);
    c.add_relay(relay, false);
    c.add_point(wm(0, 8, 0), 0);
    return c;
  };
  this->boot();
  this->create(blink("Lights", "relay_1", 120));
  this->create(blink("Boiler", "boiler", 60));
  this->engine->local = at_local(0, 8, 0);
  this->engine->tick();
  EXPECT_EQ(2u, this->engine->timers.size());
  this->engine->run(60);
  EXPECT_TRUE(this->engine->waiting("lights"));
  EXPECT_FALSE(this->engine->waiting("boiler"));
  this->engine->run(60);
  EXPECT_FALSE(this->engine->waiting("lights"));
  EXPECT_EQ((std::vector<std::string>{"boiler on", "r1 on", "boiler off", "r1 off"}), this->order) << "by id";
}

// --- What runs the list of the mode in force again, from its first action ---

TEST_F(Lists, TheFirstValidClockRunsTheListOfTheModeInForce) {
  this->start(at_local(0, 8, 2), two_modes(180, wm(0, 9, 0)));
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order);
  this->engine->run(180);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "r2 on"}), this->order) << "the wait from the boot, not from 08:00";
}

TEST_F(Lists, EnablingRunsTheList) {
  ScheduleConfig c = two_modes(60);
  c.enabled = false;
  this->start(at_local(0, 8, 2), c);
  EXPECT_TRUE(this->order.empty());
  ASSERT_TRUE(this->engine->set_enabled("lights", true).ok);
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order);
  this->engine->run(60);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "r2 on"}), this->order);
}

// A Save of an enabled schedule, a rename too, stops the list where it is and runs it again.
TEST_F(Lists, ASaveRunsTheListAgainFromItsFirstAction) {
  this->start(at_local(0, 8, 0), two_modes(120));
  this->engine->run(100);
  ScheduleConfig renamed = two_modes(120);
  renamed.name = "Garden lights";
  ASSERT_TRUE(this->engine->update("lights", renamed).ok);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "r1 on"}), this->order);
  EXPECT_EQ(1u, this->engine->timers.size()) << "one wait, the new list's";
  this->engine->run(20);
  EXPECT_EQ(2u, this->order.size()) << "the first list's wait is gone";
  this->engine->run(100);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "r1 on", "r2 on"}), this->order);
}

// Forward or back, into another mode or within the same one: its list from the first action.
TEST_F(Lists, AClockJumpRunsTheListAgain) {
  this->start(at_local(0, 8, 2), two_modes(60));
  this->engine->run(60);
  this->order.clear();
  this->engine->jump_to(at_local(0, 10, 0));
  EXPECT_EQ((std::vector<std::string>{"r1 off", "r2 off"}), this->order) << "B, in force at 10:00";
  this->order.clear();
  this->engine->jump_to(at_local(0, 8, 1));
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order) << "back into A";
  EXPECT_TRUE(this->engine->waiting("lights"));
  this->order.clear();
  this->engine->jump_to(at_local(0, 8, 3));
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order) << "two minutes on, still A: its list again";
  this->engine->run(59);
  EXPECT_EQ(1u, this->order.size()) << "the wait from the jump";
  this->engine->run(1);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "r2 on"}), this->order);
}

// The schedule that waited behind another runs its list once that one lets go of the target.
TEST_F(Lists, ATargetLetGoRunsTheListOfTheOneThatWaited) {
  ScheduleConfig lights = two_modes(60);
  lights.id = "lights";
  mkdir(this->folder().c_str(), 0755);
  write_file(this->file_of("a"), file_doc("a", "A"));
  write_file(this->file_of("lights"), to_json(lights));
  this->boot(at_local(0, 8, 2));
  EXPECT_EQ(ScheduleState::CONFLICT, this->state("lights"));
  this->order.clear();
  Result off = this->engine->set_enabled("a", false);
  EXPECT_EQ(std::vector<std::string>{"lights"}, off.started);
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order);
  this->engine->run(60);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "r2 on"}), this->order);
}

// --- What stops a list and leaves the targets as they are ---

TEST_F(Lists, DisablingStopsTheList) {
  this->start(at_local(0, 8, 0), two_modes(60));
  ASSERT_TRUE(this->engine->set_enabled("lights", false).ok);
  EXPECT_TRUE(this->engine->timers.empty());
  this->engine->run(3600);
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order);
  EXPECT_EQ("", this->engine->list_mode("lights"));
}

TEST_F(Lists, DeletingStopsTheList) {
  this->start(at_local(0, 8, 0), two_modes(60));
  ASSERT_TRUE(this->engine->remove("lights").ok);
  EXPECT_TRUE(this->engine->timers.empty());
  this->engine->run(3600);
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order);
}

TEST_F(Lists, ASaveOntoOtherTargetsStopsTheListAndRunsTheNewOne) {
  this->start(at_local(0, 8, 0), two_modes(60));
  ASSERT_TRUE(this->engine->update("lights", relay_doc("Lights", "boiler", {on_at(wm(0, 8, 0))})).ok);
  this->engine->run(3600);
  EXPECT_EQ((std::vector<std::string>{"r1 on", "boiler on"}), this->order) << "relay_2 never comes";
  ASSERT_TRUE(this->engine->update("lights", relay_doc("Lights", "relay_1", {off_at(0)}, false)).ok);
  EXPECT_EQ(2u, this->order.size()) << "saved disabled, it leaves the boiler as it is";
}

// A switch's own callback that stops the schedule stops the rest of the list too.
TEST_F(Lists, AnActionThatStopsItsScheduleStopsTheRest) {
  ScheduleConfig c;
  c.name = "Order";
  c.add_mode("go", "Go");
  c.add_relay("relay_1", true);
  c.add_relay("relay_2", true);
  c.add_point(0, 0);
  this->boot();
  this->create(c);
  this->e.relay1.on_write = [this]() { this->engine->set_enabled("order", false); };
  this->engine->local = at_local(0, 9, 0);
  this->engine->tick();
  EXPECT_TRUE(this->e.relay1.state);
  EXPECT_TRUE(this->e.relay2.writes.empty());
}

// --- A relay a running thermostat holds ---

class Held : public Lists {
 protected:
  // Floor heats on the boiler, which it holds while it runs.
  void SetUp() override {
    Lists::SetUp();
    this->add_thermostat(thermostat("Floor", "boiler"));
    this->order.clear();
  }
  // From 08:00: `actions` on the boiler and relay_1, `wait` seconds where a 0 stands.
  static ScheduleConfig list(const std::vector<int> &actions) {
    ScheduleConfig c;
    c.name = "Pump";
    c.add_mode("day", "Day");
    for (int action : actions) {
      if (action == 0) {
        c.add_wait(60);
      } else {
        c.add_relay(std::abs(action) == 1 ? "boiler" : "relay_1", action > 0);
      }
    }
    c.add_point(wm(0, 8, 0), 0);
    return c;
  }
  void let_go() { ASSERT_TRUE(hub().set_enabled("floor", false).ok); }
};

TEST_F(Held, AHeldRelayIsSkippedAndTheRestRuns) {
  this->start(at_local(0, 9, 0), list({1, 2}));
  EXPECT_EQ(std::vector<std::string>{"r1 on"}, this->order);
  EXPECT_TRUE(LogCapture::instance().has("Schedule 'Pump' left 'Boiler' alone: thermostat 'Floor' drives it"));
}

// Freed, it takes the last action on it the list has passed, at once, without waiting for the list.
TEST_F(Held, AFreedRelayTakesTheLastActionTheListPassed) {
  this->start(at_local(0, 9, 0), list({1, -1, 1, 2, 0, -1}));
  this->let_go();
  this->order.clear();
  EXPECT_EQ(1u, this->engine->run_later());
  EXPECT_EQ(std::vector<std::string>{"boiler on"}, this->order);
  this->engine->run(60);
  EXPECT_EQ((std::vector<std::string>{"boiler on", "boiler off"}), this->order) << "then the list goes on";
}

TEST_F(Held, ARelayTheListHasNotReachedIsLeftToTheList) {
  this->start(at_local(0, 9, 0), list({0, 1}));
  this->let_go();
  this->order.clear();
  this->engine->run_later();
  EXPECT_TRUE(this->order.empty());
  this->engine->run(60);
  EXPECT_EQ(std::vector<std::string>{"boiler on"}, this->order);
}

TEST_F(Held, AfterAFinishedListTheWholeListCounts) {
  this->start(at_local(0, 9, 0), list({1, 2, -1}));
  this->let_go();
  this->order.clear();
  this->engine->run_later();
  EXPECT_EQ(std::vector<std::string>{"boiler off"}, this->order);
}

TEST_F(Held, ARelayOfAScheduleThatStoppedIsNotSet) {
  this->start(at_local(0, 9, 0), list({1}));
  ASSERT_TRUE(this->engine->set_enabled("pump", false).ok);
  this->let_go();
  this->order.clear();
  EXPECT_EQ(0u, this->engine->run_later());
  EXPECT_TRUE(this->order.empty());
}

// Let go, then the list starts over before the loop gets to it: the new list sets the relay itself.
TEST_F(Held, ARelayLetGoBeforeANewListIsTheNewListsToSet) {
  this->start(at_local(0, 9, 0), list({1}));
  this->let_go();
  this->order.clear();
  ASSERT_TRUE(this->engine->update("pump", list({0, -1})).ok);
  EXPECT_EQ(1u, this->engine->run_later());
  EXPECT_TRUE(this->order.empty()) << "the first list's on is not set";
  this->engine->run(60);
  EXPECT_EQ(std::vector<std::string>{"boiler off"}, this->order);
}

}  // namespace esphome::schedules::testing
