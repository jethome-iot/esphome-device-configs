#include <gtest/gtest.h>

#include "common.h"

namespace esphome::display_menu_base::testing {

// Root: Sub, then Custom. Sub holds Leaf, an editable Editable, and a back row, so a case can
// tell "left the submenu" apart from "ended the edit and stayed".
class Navigation : public ::testing::Test {
 protected:
  void SetUp() override {
    this->submenu_.set_text("Sub");
    this->leaf_.set_text("Leaf");
    this->editable_.set_text("Editable");
    this->back_row_.set_text("Back");
    this->submenu_.add_item(&this->leaf_);
    this->submenu_.add_item(&this->editable_);
    this->submenu_.add_item(&this->back_row_);

    this->custom_.set_text("Custom");
    this->custom_.add_on_next_callback([this]() { this->next_calls_++; });
    this->root_.add_item(&this->submenu_);
    this->root_.add_item(&this->custom_);

    this->menu_.set_root_item(&this->root_);
  }

  const std::vector<std::string> root_rows_{"Sub", "Custom"};
  const std::vector<std::string> submenu_rows_{"Leaf", "Editable", "Back"};

  MenuItemMenu root_;
  MenuItemMenu submenu_;
  MenuItem leaf_{MENU_ITEM_LABEL};
  MenuItemCustom editable_;
  MenuItem back_row_{MENU_ITEM_BACK};
  MenuItemCustom custom_;
  RecordingMenu menu_;
  int next_calls_{0};
};

TEST_F(Navigation, RightEntersASubmenuByDefault) {
  this->menu_.right();

  EXPECT_FALSE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->submenu_rows_);
  EXPECT_EQ(this->menu_.selected_row, 0);
}

TEST_F(Navigation, RightLeavesASubmenuAloneWhenTheOptionIsOff) {
  this->menu_.set_right_for_menu_enter_opt(false);

  this->menu_.right();
  this->menu_.draw();

  EXPECT_TRUE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->root_rows_);
}

// The option exists so RIGHT can stay a value key; turning it off must not cost that.
TEST_F(Navigation, RightStillEditsAValueWhenTheOptionIsOff) {
  this->menu_.set_right_for_menu_enter_opt(false);
  this->custom_.set_immediate_edit(true);

  this->menu_.down();  // onto Custom
  this->menu_.right();

  EXPECT_EQ(this->next_calls_, 1);
}

TEST_F(Navigation, EnterStillOpensASubmenuWhenRightIsOff) {
  this->menu_.set_right_for_menu_enter_opt(false);

  this->menu_.enter();

  EXPECT_FALSE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->submenu_rows_);
}

TEST_F(Navigation, BackLeavesTheSubmenu) {
  this->menu_.enter();
  ASSERT_FALSE(this->menu_.is_at_main());

  EXPECT_TRUE(this->menu_.back());

  EXPECT_TRUE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->root_rows_);
}

TEST_F(Navigation, BackAtTheRootReportsThatItWentNowhere) {
  EXPECT_FALSE(this->menu_.back());

  EXPECT_TRUE(this->menu_.is_at_main());
}

TEST_F(Navigation, BackEndsAnEditBeforeItLeavesTheMenu) {
  this->menu_.enter();  // into Sub
  this->menu_.down();   // onto Editable
  this->menu_.enter();  // which starts editing it

  EXPECT_TRUE(this->menu_.back());  // ends the edit and stays in the submenu
  EXPECT_FALSE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->submenu_rows_);

  EXPECT_TRUE(this->menu_.back());  // only now does it leave
  EXPECT_TRUE(this->menu_.is_at_main());
}

TEST_F(Navigation, BackDoesNothingWhileTheMenuIsHidden) {
  this->menu_.enter();
  ASSERT_FALSE(this->menu_.is_at_main());

  this->menu_.set_active(false);

  EXPECT_FALSE(this->menu_.back());
  EXPECT_FALSE(this->menu_.is_at_main());  // and it did not quietly leave the submenu
}

}  // namespace esphome::display_menu_base::testing
