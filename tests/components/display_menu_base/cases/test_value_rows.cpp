#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "common.h"

namespace esphome::display_menu_base::testing {

// Optimistic like a template select: what the menu sends becomes the state.
class FakeSelect : public select::Select {
 public:
  FakeSelect() {
    this->configure_entity_("Network mode", 0, 0);
    this->traits.set_options({"Auto", "Ethernet", "WiFi"});
  }

  int writes{0};

 protected:
  void control(size_t index) override {
    this->writes++;
    this->publish_state(index);
  }
};

class FakeNumber : public number::Number {
 public:
  FakeNumber() {
    this->configure_entity_("Modbus address", 0, 0);
    this->traits.set_min_value(1);
    this->traits.set_max_value(247);
    this->traits.set_step(1);
  }

  int writes{0};

 protected:
  void control(float value) override {
    this->writes++;
    this->publish_state(value);
  }
};

// A stock select row and a stock number row: an edit opened with CENTER steps the value on the
// row only, and the entity gets it once, when the edit ends.
class ValueRows : public ::testing::Test {
 protected:
  void SetUp() override {
    this->select_.publish_state(size_t{1});  // Ethernet
    this->select_row_.set_text("Network");
    this->select_row_.set_select_variable(&this->select_);
    this->select_row_.add_on_value_callback([this]() { this->select_on_value_++; });

    this->number_.publish_state(1);
    this->number_row_.set_text("Address");
    this->number_row_.set_number_variable(&this->number_);
    this->number_row_.set_format("%.0f");
    this->number_row_.add_on_value_callback([this]() { this->number_on_value_++; });

    this->root_.add_item(&this->select_row_);
    this->root_.add_item(&this->number_row_);
    this->menu_.set_root_item(&this->root_);
  }

  std::string option() const { return std::string(this->select_.current_option().c_str()); }
  std::string select_shown() const { return this->select_row_.get_value_text(); }
  std::string number_shown() const { return this->number_row_.get_value_text(); }
  void on_number_row() { this->menu_.down(); }

  MenuItemMenu root_;
  MenuItemSelect select_row_;
  MenuItemNumber number_row_;
  FakeSelect select_;
  FakeNumber number_;
  RecordingMenu menu_;
  int select_on_value_{0};
  int number_on_value_{0};
};

TEST_F(ValueRows, ASelectStepReachesTheSelectOnlyWhenTheEditEnds) {
  this->menu_.enter();
  this->menu_.right();

  EXPECT_EQ(this->select_shown(), "WiFi");  // the row already shows the choice
  EXPECT_EQ(this->option(), "Ethernet");    // but the select still has the old one
  EXPECT_EQ(this->select_.writes, 0);
  EXPECT_EQ(this->select_on_value_, 0);

  this->menu_.enter();

  EXPECT_EQ(this->option(), "WiFi");
  EXPECT_EQ(this->select_.writes, 1);
  EXPECT_EQ(this->select_on_value_, 1);
  EXPECT_EQ(this->select_shown(), "WiFi");
}

TEST_F(ValueRows, ASelectEditCyclesBothWays) {
  this->menu_.enter();
  this->menu_.right();
  this->menu_.right();

  EXPECT_EQ(this->select_shown(), "Auto");

  this->menu_.left();

  EXPECT_EQ(this->select_shown(), "WiFi");
  EXPECT_EQ(this->select_.writes, 0);
}

TEST_F(ValueRows, AnEditBackToTheStartWritesNothing) {
  this->menu_.enter();
  this->menu_.right();
  this->menu_.left();
  this->menu_.enter();

  EXPECT_EQ(this->select_.writes, 0);
  EXPECT_EQ(this->select_on_value_, 0);
}

// The Network row on the panel and the dashboard drive one select. Opening the row and closing
// it without a step must not put back what the row showed when it opened.
TEST_F(ValueRows, AnEditWithoutAStepKeepsAChangeMadeElsewhere) {
  this->menu_.enter();
  this->select_.publish_state(size_t{2});

  EXPECT_EQ(this->select_shown(), "WiFi");

  this->menu_.enter();

  EXPECT_EQ(this->option(), "WiFi");
  EXPECT_EQ(this->select_.writes, 0);
}

// Once the row has stepped, it is the choice on screen that the select gets.
TEST_F(ValueRows, TheChoiceOnScreenWinsOverAChangeMadeElsewhere) {
  this->menu_.enter();
  this->menu_.left();  // Auto
  this->select_.publish_state(size_t{2});
  this->menu_.enter();

  EXPECT_EQ(this->option(), "Auto");
  EXPECT_EQ(this->select_.writes, 1);
}

TEST_F(ValueRows, BackAppliesTheChoice) {
  this->menu_.enter();
  this->menu_.right();
  this->menu_.back();

  EXPECT_EQ(this->option(), "WiFi");
  EXPECT_EQ(this->select_.writes, 1);
  EXPECT_TRUE(this->menu_.is_at_main());  // it ended the edit, it did not leave the menu
}

// HOME and the display-off timer hide the menu mid-edit: the choice is applied, once.
TEST_F(ValueRows, HidingTheMenuAppliesTheChoiceOnce) {
  this->menu_.enter();
  this->menu_.right();
  this->menu_.hide();
  this->menu_.hide();

  EXPECT_EQ(this->option(), "WiFi");
  EXPECT_EQ(this->select_.writes, 1);
}

TEST_F(ValueRows, ShowMainMidEditAppliesTheChoice) {
  this->menu_.enter();
  this->menu_.right();
  this->menu_.show_main();

  EXPECT_EQ(this->option(), "WiFi");
  EXPECT_EQ(this->select_.writes, 1);
}

// A row's own on_leave runs after the select has the choice, so it can act on it.
TEST_F(ValueRows, OnLeaveSeesTheAppliedOption) {
  std::string seen;
  int on_value_seen = -1;
  this->select_row_.add_on_leave_callback([&]() {
    seen = this->option();
    on_value_seen = this->select_on_value_;
  });

  this->menu_.enter();
  this->menu_.right();
  this->menu_.enter();

  EXPECT_EQ(seen, "WiFi");
  EXPECT_EQ(on_value_seen, 1);
}

// The write runs the entity's and the row's callbacks; one that hides the menu (a config
// leaving the menu once a setting is made) must not end the same edit again.
TEST_F(ValueRows, AnOnValueThatHidesTheMenuEndsTheEditOnce) {
  int leaves = 0;
  this->select_row_.add_on_value_callback([this]() { this->menu_.hide(); });
  this->select_row_.add_on_leave_callback([&]() { leaves++; });

  this->menu_.enter();
  this->menu_.right();
  this->menu_.enter();

  EXPECT_EQ(this->option(), "WiFi");
  EXPECT_EQ(this->select_.writes, 1);
  EXPECT_EQ(leaves, 1);
}

TEST_F(ValueRows, AnOnValueThatShowsMainEndsANumberEditOnce) {
  int leaves = 0;
  this->number_row_.add_on_value_callback([this]() { this->menu_.show_main(); });
  this->number_row_.add_on_leave_callback([&]() { leaves++; });

  this->on_number_row();
  this->menu_.enter();
  this->menu_.right();
  this->menu_.enter();

  EXPECT_EQ(this->number_.state, 2);
  EXPECT_EQ(this->number_.writes, 1);
  EXPECT_EQ(leaves, 1);
}

// immediate_edit means "no edit to open": every step still goes straight to the select.
TEST_F(ValueRows, AnImmediateSelectRowAppliesEveryStep) {
  this->select_row_.set_immediate_edit(true);

  this->menu_.right();

  EXPECT_EQ(this->option(), "WiFi");
  EXPECT_EQ(this->select_.writes, 1);
  EXPECT_EQ(this->select_on_value_, 1);

  this->menu_.right();

  EXPECT_EQ(this->option(), "Auto");
  EXPECT_EQ(this->select_.writes, 2);

  this->menu_.left();

  EXPECT_EQ(this->option(), "WiFi");
  EXPECT_EQ(this->select_.writes, 3);
}

// Without a state SelectCall's first step lands on an end; the pending one does the same.
TEST_F(ValueRows, ASelectWithoutAStateStartsAtAnEnd) {
  FakeSelect left;
  this->select_row_.set_select_variable(&left);
  this->menu_.enter();
  this->menu_.left();

  EXPECT_EQ(this->select_shown(), "WiFi");

  this->menu_.enter();

  EXPECT_EQ(std::string(left.current_option().c_str()), "WiFi");
  EXPECT_EQ(left.writes, 1);

  FakeSelect right;
  this->select_row_.set_select_variable(&right);
  this->menu_.enter();
  this->menu_.right();

  EXPECT_EQ(this->select_shown(), "Auto");
}

TEST_F(ValueRows, ASelectWithoutOptionsIsInert) {
  FakeSelect empty;
  empty.traits.set_options({});
  this->select_row_.set_select_variable(&empty);

  this->menu_.enter();
  this->menu_.right();
  this->menu_.enter();

  EXPECT_EQ(this->select_shown(), "");
  EXPECT_EQ(empty.writes, 0);
}

TEST_F(ValueRows, TheRotaryModeStepsAndAppliesTheSameWay) {
  this->menu_.set_mode(MENU_MODE_ROTARY);

  this->menu_.enter();
  this->menu_.down();

  EXPECT_EQ(this->select_shown(), "WiFi");
  EXPECT_EQ(this->select_.writes, 0);

  this->menu_.left();  // ends the edit in the rotary mode

  EXPECT_EQ(this->option(), "WiFi");
  EXPECT_EQ(this->select_.writes, 1);
}

TEST_F(ValueRows, ANumberStepReachesTheNumberOnlyWhenTheEditEnds) {
  this->on_number_row();
  this->menu_.enter();
  this->menu_.right();
  this->menu_.right();

  EXPECT_EQ(this->number_shown(), "3");
  EXPECT_EQ(this->number_.state, 1);
  EXPECT_EQ(this->number_.writes, 0);
  EXPECT_EQ(this->number_on_value_, 0);

  this->menu_.enter();

  EXPECT_EQ(this->number_.state, 3);
  EXPECT_EQ(this->number_.writes, 1);
  EXPECT_EQ(this->number_on_value_, 1);
}

TEST_F(ValueRows, ANumberStopsAtItsLimits) {
  this->on_number_row();
  this->menu_.enter();
  this->menu_.left();

  EXPECT_EQ(this->number_shown(), "1");

  this->menu_.enter();

  EXPECT_EQ(this->number_.writes, 0);

  this->number_.publish_state(246);
  this->menu_.enter();
  this->menu_.right();
  this->menu_.right();

  EXPECT_EQ(this->number_shown(), "247");

  this->menu_.enter();

  EXPECT_EQ(this->number_.state, 247);
  EXPECT_EQ(this->number_.writes, 1);
}

TEST_F(ValueRows, ANumberEditBackToTheStartWritesNothing) {
  this->on_number_row();
  this->menu_.enter();
  this->menu_.right();
  this->menu_.left();
  this->menu_.back();

  EXPECT_EQ(this->number_.writes, 0);
  EXPECT_EQ(this->number_on_value_, 0);
}

// A step size the number does not set is 1, as NumberCall takes it.
TEST_F(ValueRows, ANumberWithoutAStepStepsByOne) {
  this->number_.traits.set_step(NAN);

  this->on_number_row();
  this->menu_.enter();
  this->menu_.right();
  this->menu_.enter();

  EXPECT_EQ(this->number_.state, 2);
}

// No state, nothing to step from: the row reads the minimum and every step is inert.
TEST_F(ValueRows, ANumberWithoutAStateIsInert) {
  FakeNumber fresh;
  this->number_row_.set_number_variable(&fresh);

  this->on_number_row();
  this->menu_.enter();
  this->menu_.right();

  EXPECT_EQ(this->number_shown(), "1");

  this->menu_.enter();

  EXPECT_EQ(fresh.writes, 0);
}

TEST_F(ValueRows, AnImmediateNumberRowAppliesEveryStep) {
  this->number_row_.set_immediate_edit(true);

  this->on_number_row();
  this->menu_.right();

  EXPECT_EQ(this->number_.state, 2);
  EXPECT_EQ(this->number_.writes, 1);
  EXPECT_EQ(this->number_on_value_, 1);

  this->menu_.left();

  EXPECT_EQ(this->number_.state, 1);
  EXPECT_EQ(this->number_.writes, 2);
}

// A state past the limits (a restored value from before the limits changed) reads as the
// limit, and steps start from what the row shows, so the first one is seen to move.
TEST_F(ValueRows, ANumberPastItsLimitsStepsFromTheLimit) {
  this->number_.publish_state(300);

  EXPECT_EQ(this->number_shown(), "247");

  this->on_number_row();
  this->menu_.enter();
  this->menu_.right();

  EXPECT_EQ(this->number_shown(), "247");

  this->menu_.enter();

  EXPECT_EQ(this->number_.writes, 0);

  this->menu_.enter();
  this->menu_.left();

  EXPECT_EQ(this->number_shown(), "246");

  this->menu_.enter();

  EXPECT_EQ(this->number_.state, 246);
  EXPECT_EQ(this->number_.writes, 1);
}

// A float sum of 0.1 steps drifts: up three and down three must still land on the start.
TEST_F(ValueRows, AFractionalStepBackToTheStartWritesNothing) {
  this->number_.traits.set_max_value(100);
  this->number_.traits.set_step(0.1f);
  this->number_.publish_state(15.7f);

  this->on_number_row();
  this->menu_.enter();
  for (int i = 0; i < 3; i++)
    this->menu_.right();
  for (int i = 0; i < 3; i++)
    this->menu_.left();
  this->menu_.enter();

  EXPECT_EQ(this->number_.writes, 0);
}

// The rotary mode opens an edit on an immediate number all the same; it keeps every step.
TEST_F(ValueRows, AnImmediateNumberRowAppliesEveryStepInTheRotaryMode) {
  this->menu_.set_mode(MENU_MODE_ROTARY);
  this->number_row_.set_immediate_edit(true);

  this->on_number_row();
  this->menu_.enter();
  this->menu_.down();

  EXPECT_EQ(this->number_.state, 2);
  EXPECT_EQ(this->number_.writes, 1);

  this->menu_.left();

  EXPECT_EQ(this->number_.writes, 1);
}

}  // namespace esphome::display_menu_base::testing
