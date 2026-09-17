#include <gtest/gtest.h>

#include "common.h"

namespace esphome::display_menu_base::testing {

// Root: an empty submenu, a filled one and a label. The root itself is never empty: the schema
// refuses that config, and the guard under test does not cover it.
class EmptyMenu : public ::testing::Test {
 protected:
  void SetUp() override {
    this->empty_.set_text("Empty");
    this->empty_.add_on_enter_callback([this]() { this->empty_enters_++; });

    this->leaf_.set_text("Leaf");
    this->back_row_.set_text("Back");
    this->filled_.set_text("Filled");
    this->filled_.add_item(&this->leaf_);
    this->filled_.add_item(&this->back_row_);

    this->label_.set_text("Label");

    this->root_.add_item(&this->empty_);
    this->root_.add_item(&this->filled_);
    this->root_.add_item(&this->label_);
    this->root_.add_on_leave_callback([this]() { this->root_leaves_++; });

    this->menu_.set_root_item(&this->root_);
  }

  const std::vector<std::string> root_rows_{"Empty", "Filled", "Label"};
  const std::vector<std::string> filled_rows_{"Leaf", "Back"};

  MenuItemMenu root_;
  MenuItemMenu empty_;
  MenuItemMenu filled_;
  MenuItem leaf_{MENU_ITEM_LABEL};
  MenuItem back_row_{MENU_ITEM_BACK};
  MenuItem label_{MENU_ITEM_LABEL};
  RecordingMenu menu_;
  int empty_enters_{0};
  int root_leaves_{0};
};

TEST_F(EmptyMenu, EnterDoesNotOpenIt) {
  this->menu_.enter();
  this->menu_.draw();

  EXPECT_TRUE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->root_rows_);
  EXPECT_EQ(this->menu_.selected_row, 0);
}

TEST_F(EmptyMenu, RightDoesNotOpenIt) {
  this->menu_.right();
  this->menu_.draw();

  EXPECT_TRUE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->root_rows_);
  EXPECT_EQ(this->menu_.selected_row, 0);
}

// The refusal has to come before the on_leave(), or the menu we stay in gets a leave with no
// enter to match it.
TEST_F(EmptyMenu, RefusingItRunsNoEnterOrLeaveCallback) {
  this->menu_.enter();
  this->menu_.right();

  EXPECT_EQ(this->root_leaves_, 0);
  EXPECT_EQ(this->empty_enters_, 0);
}

TEST_F(EmptyMenu, EveryKeyIsSafeWhileItIsSelected) {
  this->menu_.enter();  // refused
  this->menu_.up();
  this->menu_.left();
  this->menu_.right();  // refused again
  this->menu_.enter();  // refused again
  EXPECT_FALSE(this->menu_.back());
  this->menu_.down();  // and the cursor still moves

  EXPECT_TRUE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->root_rows_);
  EXPECT_EQ(this->menu_.selected_row, 1);
}

// The guard reads items_size() on every press rather than remembering the menu was empty, which
// is what lets a package or a boot lambda fill it later.
TEST_F(EmptyMenu, ItOpensOnceSomethingFillsIt) {
  this->menu_.enter();
  ASSERT_TRUE(this->menu_.is_at_main());

  MenuItem row{MENU_ITEM_LABEL};
  row.set_text("Filled in later");
  this->empty_.add_item(&row);

  EXPECT_TRUE(this->menu_.is_at_main());  // nothing happens on its own
  this->menu_.enter();

  EXPECT_FALSE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, (std::vector<std::string>{"Filled in later"}));
  EXPECT_EQ(this->empty_enters_, 1);
  EXPECT_TRUE(this->menu_.back());
  EXPECT_TRUE(this->menu_.is_at_main());
}

TEST_F(EmptyMenu, AFilledSubmenuStillOpensAfterwards) {
  this->menu_.enter();  // refused
  this->menu_.down();   // onto Filled
  this->menu_.enter();

  EXPECT_FALSE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->filled_rows_);
  EXPECT_EQ(this->menu_.selected_row, 0);

  EXPECT_TRUE(this->menu_.back());
  EXPECT_TRUE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.selected_row, 1);  // the cursor came back where it was
}

}  // namespace esphome::display_menu_base::testing
