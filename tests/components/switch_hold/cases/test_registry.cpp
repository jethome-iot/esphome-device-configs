#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>
#include "esphome/components/switch/switch.h"
#include "esphome/components/switch_hold/switch_hold.h"

namespace esphome::switch_hold::testing {

class FakeSwitch : public switch_::Switch {
 protected:
  void write_state(bool state) override { this->publish_state(state); }
};

// Answers from a table the case fills.
class FakeHolder : public SwitchHolder {
 public:
  std::map<const switch_::Switch *, std::string> held;
  std::string holder_of(const switch_::Switch *sw) const override {
    auto it = this->held.find(sw);
    return it == this->held.end() ? std::string() : it->second;
  }
};

// The registry is process-wide, as on a device: every case leaves it with no holder.
class Registry : public ::testing::Test {
 protected:
  void TearDown() override { set_holder(nullptr); }
  FakeSwitch relay1;
  FakeSwitch relay2;
};

TEST_F(Registry, NothingRegisteredHoldsNothing) {
  EXPECT_EQ("", holder(&this->relay1));
  EXPECT_EQ("", holder(nullptr));
}

TEST_F(Registry, TheHolderAnswersPerSwitch) {
  FakeHolder hub;
  hub.held[&this->relay1] = "Living room";
  set_holder(&hub);
  EXPECT_EQ("Living room", holder(&this->relay1));
  EXPECT_EQ("", holder(&this->relay2));
  EXPECT_EQ("", holder(nullptr)) << "no switch, no question";
}

TEST_F(Registry, ALaterHolderReplacesTheFirst) {
  FakeHolder first;
  first.held[&this->relay1] = "First";
  FakeHolder second;
  second.held[&this->relay2] = "Second";
  set_holder(&first);
  set_holder(&second);
  EXPECT_EQ("", holder(&this->relay1));
  EXPECT_EQ("Second", holder(&this->relay2));
}

// Every listener hears every release; the registry keeps them for the life of the process.
TEST_F(Registry, AReleaseReachesEveryListener) {
  static std::vector<switch_::Switch *> heard_a;
  static std::vector<switch_::Switch *> heard_b;
  static bool added = false;
  if (!added) {
    add_on_release_callback([](switch_::Switch *sw) { heard_a.push_back(sw); });
    add_on_release_callback([](switch_::Switch *sw) { heard_b.push_back(sw); });
    added = true;
  }
  heard_a.clear();
  heard_b.clear();

  notify_released(&this->relay2);
  notify_released(&this->relay1);
  EXPECT_EQ((std::vector<switch_::Switch *>{&this->relay2, &this->relay1}), heard_a);
  EXPECT_EQ(heard_a, heard_b);
}

}  // namespace esphome::switch_hold::testing
