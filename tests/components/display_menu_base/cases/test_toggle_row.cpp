#include <gtest/gtest.h>

#include "common.h"

namespace esphome::display_menu_base::testing {

// The Automations rows are MenuItemCustom two-state toggles: the flip is held in a pending
// value and applied when the edit ends, because applying it per keypress would rewrite the
// rule's file on every press. This pins that contract, not any one menu that uses it.
class ToggleRow : public ::testing::Test {
 protected:
  void SetUp() override {
    this->row_.set_text("Porch light");
    this->row_.set_value_lambda([this](const MenuItem *) {
      const int state = this->pending_ >= 0 ? this->pending_ : this->state_of_();
      return std::string(state < 0 ? "--" : state > 0 ? "On" : "Off");
    });
    this->row_.add_on_enter_callback([this]() { this->pending_ = this->state_of_(); });
    auto flip = [this]() {
      if (this->pending_ >= 0)
        this->pending_ = 1 - this->pending_;
    };
    this->row_.add_on_next_callback(flip);
    this->row_.add_on_prev_callback(flip);
    this->row_.add_on_leave_callback([this]() {
      const int current = this->state_of_();
      if (this->pending_ >= 0 && current >= 0 && this->pending_ != current && this->set_enabled_(this->pending_ > 0))
        this->set_enabled_(this->pending_ == 0);
      this->pending_ = -1;
    });

    this->root_.add_item(&this->row_);
    this->menu_.set_root_item(&this->root_);
  }

  /// The engine's lookup by id: -1 once the rule is gone.
  int state_of_() const { return this->gone_ ? -1 : (this->stored_ ? 1 : 0); }
  /// set_enable_automation: live either way, true when the file was not written.
  bool set_enabled_(bool enable) {
    this->stored_ = enable;
    this->writes_++;
    return this->write_refused_;
  }
  std::string value() const { return this->row_.get_value_text(); }

  MenuItemMenu root_;
  MenuItemCustom row_;
  RecordingMenu menu_;
  bool stored_{false};
  bool gone_{false};
  bool write_refused_{false};
  int pending_{-1};
  int writes_{0};
};

TEST_F(ToggleRow, AFlipReachesTheStoreOnlyWhenTheEditEnds) {
  this->menu_.enter();
  this->menu_.right();

  EXPECT_EQ(this->value(), "On");  // the row already shows the choice
  EXPECT_FALSE(this->stored_);     // but nothing was written yet

  this->menu_.enter();

  EXPECT_TRUE(this->stored_);
  EXPECT_EQ(this->writes_, 1);
  EXPECT_EQ(this->value(), "On");
}

TEST_F(ToggleRow, LeftFlipsTheSameWayAsRight) {
  this->menu_.enter();
  this->menu_.left();
  this->menu_.enter();

  EXPECT_TRUE(this->stored_);
  EXPECT_EQ(this->writes_, 1);
}

// Stepping back and forth lands on the value it started from, so there is nothing to write.
TEST_F(ToggleRow, AnEditThatChangesNothingWritesNothing) {
  this->menu_.enter();
  this->menu_.right();
  this->menu_.right();
  this->menu_.enter();

  EXPECT_FALSE(this->stored_);
  EXPECT_EQ(this->writes_, 0);
}

TEST_F(ToggleRow, OpeningAndClosingWithoutSteppingWritesNothing) {
  this->menu_.enter();
  this->menu_.enter();

  EXPECT_EQ(this->writes_, 0);
  EXPECT_EQ(this->value(), "Off");
}

// BACK ends the edit the same way CENTER does; the choice must not be dropped.
TEST_F(ToggleRow, BackAppliesTheChoiceToo) {
  this->menu_.enter();
  this->menu_.right();
  this->menu_.back();

  EXPECT_TRUE(this->stored_);
  EXPECT_EQ(this->writes_, 1);
}

// HOME and the display-off timer hide the menu mid-edit; that ends the edit as well, so the
// choice is applied rather than lost, and hiding again does not write it twice.
TEST_F(ToggleRow, HidingTheMenuAppliesTheChoiceOnce) {
  this->menu_.enter();
  this->menu_.right();
  this->menu_.hide();
  this->menu_.hide();

  EXPECT_TRUE(this->stored_);
  EXPECT_EQ(this->writes_, 1);
}

// A rule removed over HTTP leaves its row behind: it reads `--` and every key is inert.
TEST_F(ToggleRow, ARowWhoseRuleIsGoneIsInert) {
  this->gone_ = true;

  EXPECT_EQ(this->value(), "--");

  this->menu_.enter();
  this->menu_.right();

  EXPECT_EQ(this->value(), "--");

  this->menu_.enter();

  EXPECT_EQ(this->writes_, 0);
}

// The rule goes while the row is open: the pending choice must not be written over a gap.
TEST_F(ToggleRow, ARuleThatGoesMidEditIsNotWrittenBack) {
  this->menu_.enter();
  this->menu_.right();
  this->gone_ = true;
  this->menu_.enter();

  EXPECT_EQ(this->writes_, 0);
}

// A rule naming an entity that is gone is live-only: the engine refuses the file. The row has
// to put the live state back, or it would promise a change the next boot undoes.
TEST_F(ToggleRow, AChoiceTheEngineCannotWriteSpringsBack) {
  this->write_refused_ = true;

  this->menu_.enter();
  this->menu_.right();
  this->menu_.enter();

  EXPECT_FALSE(this->stored_);
  EXPECT_EQ(this->value(), "Off");
}

}  // namespace esphome::display_menu_base::testing
