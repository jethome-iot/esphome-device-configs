#include <gtest/gtest.h>

#include <memory>

#include "common.h"

namespace esphome::display_menu_base::testing {

// A submenu that refills itself from its own on_enter, as the Automations list does: rows come
// from a pool that only grows and are reused by position, and with nothing to list it shows one
// placeholder, since an empty submenu cannot be entered. Root: Sub, then Other.
class Refill : public ::testing::Test {
 protected:
  void SetUp() override {
    this->submenu_.set_text("Sub");
    this->placeholder_.set_text("Nothing");
    this->submenu_.add_on_enter_callback([this]() {
      this->enters_++;
      this->events_.push_back("open");
      this->refill_();
    });
    this->submenu_.add_on_leave_callback([this]() { this->leaves_++; });
    this->refill_();  // an empty submenu could not be entered to fill it

    this->other_.set_text("Other");
    this->root_.add_item(&this->submenu_);
    this->root_.add_item(&this->other_);
    this->root_.add_on_leave_callback([this]() { this->root_leaves_++; });

    this->menu_.set_root_item(&this->root_);
  }

  void refill_() {
    this->submenu_.clear_items();
    if (this->with_editable_)
      this->submenu_.add_item(&this->editable_);
    size_t used = 0;
    for (const auto &name : this->names_) {
      if (used == this->pool_.size())
        this->pool_.push_back(std::make_unique<MenuItem>(MENU_ITEM_LABEL));
      MenuItem *row = this->pool_[used++].get();
      row->set_text(name);
      this->submenu_.add_item(row);
    }
    if (this->submenu_.items_size() == 0)
      this->submenu_.add_item(&this->placeholder_);
  }

  const std::vector<std::string> root_rows_{"Sub", "Other"};

  MenuItemMenu root_;
  MenuItemMenu submenu_;
  MenuItem other_{MENU_ITEM_LABEL};
  MenuItem placeholder_{MENU_ITEM_LABEL};
  MenuItemCustom editable_;
  bool with_editable_{false};
  std::vector<std::string> events_;
  std::vector<std::unique_ptr<MenuItem>> pool_;
  std::vector<std::string> names_{"First"};
  RecordingMenu menu_;
  int enters_{0};
  int leaves_{0};
  int root_leaves_{0};
};

TEST_F(Refill, TheDrawAfterEnterShowsWhatOnEnterPutThere) {
  this->names_ = {"Alpha", "Beta"};  // after SetUp listed First
  this->menu_.enter();

  EXPECT_FALSE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, (std::vector<std::string>{"Alpha", "Beta"}));
  EXPECT_EQ(this->menu_.selected_row, 0);
}

// The cursor is left on a row that the next refill no longer has.
TEST_F(Refill, FewerRowsThanLastTime) {
  this->names_ = {"Alpha", "Beta", "Gamma"};
  this->menu_.enter();
  this->menu_.down();
  this->menu_.down();
  ASSERT_EQ(this->menu_.selected_row, 2);
  ASSERT_TRUE(this->menu_.back());

  this->names_ = {"Alpha"};
  this->menu_.enter();

  EXPECT_EQ(this->menu_.rows, (std::vector<std::string>{"Alpha"}));
  EXPECT_EQ(this->menu_.selected_row, 0);

  this->menu_.down();  // nothing below
  this->menu_.draw();
  EXPECT_EQ(this->menu_.selected_row, 0);
}

TEST_F(Refill, MoreRowsThanLastTime) {
  this->menu_.enter();
  ASSERT_EQ(this->menu_.rows, (std::vector<std::string>{"First"}));
  ASSERT_TRUE(this->menu_.back());

  this->names_ = {"First", "Second", "Third"};
  this->menu_.enter();

  EXPECT_EQ(this->menu_.rows, (std::vector<std::string>{"First", "Second", "Third"}));
  this->menu_.down();
  EXPECT_EQ(this->menu_.selected_row, 1);
  this->menu_.down();
  EXPECT_EQ(this->menu_.selected_row, 2);
  this->menu_.down();  // the last row stops it
  this->menu_.draw();
  EXPECT_EQ(this->menu_.selected_row, 2);
}

TEST_F(Refill, NothingToListShowsThePlaceholder) {
  this->names_ = {"Alpha", "Beta"};
  this->menu_.enter();
  ASSERT_TRUE(this->menu_.back());

  this->names_.clear();
  this->menu_.enter();

  EXPECT_FALSE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, (std::vector<std::string>{"Nothing"}));
  EXPECT_EQ(this->menu_.selected_row, 0);
  EXPECT_TRUE(this->menu_.back());  // and the way out still works
  EXPECT_TRUE(this->menu_.is_at_main());
}

// The first rule on a device that had none.
TEST_F(Refill, ThePlaceholderMakesWayForTheFirstRow) {
  this->names_.clear();
  this->menu_.enter();
  ASSERT_EQ(this->menu_.rows, (std::vector<std::string>{"Nothing"}));
  ASSERT_TRUE(this->menu_.back());

  this->names_ = {"Alpha"};
  this->menu_.enter();

  EXPECT_EQ(this->menu_.rows, (std::vector<std::string>{"Alpha"}));
  EXPECT_EQ(this->menu_.selected_row, 0);
  EXPECT_EQ(this->placeholder_.get_parent(), nullptr);
}

TEST_F(Refill, ClearItemsDetachesAndAddItemReattaches) {
  MenuItem *first = this->submenu_.get_item(0);
  ASSERT_EQ(first->get_parent(), &this->submenu_);

  this->submenu_.clear_items();

  EXPECT_EQ(this->submenu_.items_size(), 0u);
  EXPECT_EQ(first->get_parent(), nullptr);

  this->submenu_.add_item(first);

  EXPECT_EQ(this->submenu_.items_size(), 1u);
  EXPECT_EQ(first->get_parent(), &this->submenu_);
}

// Emptied outside its own on_enter, the submenu gets the empty-menu refusal, so the on_enter
// that would have refilled it never runs.
TEST_F(Refill, ClearedAndLeftEmptyItIsRefused) {
  this->submenu_.clear_items();

  this->menu_.enter();
  this->menu_.right();
  this->menu_.draw();

  EXPECT_TRUE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->root_rows_);
  EXPECT_EQ(this->enters_, 0);
  EXPECT_EQ(this->leaves_, 0);
  EXPECT_EQ(this->root_leaves_, 0);
}

// HOME and the display-off timer leave the submenu without a back press.
TEST_F(Refill, ItRefillsAgainAfterHideAndShowMain) {
  this->menu_.enter();
  this->menu_.hide();
  this->menu_.show_main();

  EXPECT_TRUE(this->menu_.is_at_main());
  EXPECT_EQ(this->menu_.rows, this->root_rows_);

  this->names_ = {"Alpha", "Beta"};
  this->menu_.enter();

  EXPECT_EQ(this->menu_.rows, (std::vector<std::string>{"Alpha", "Beta"}));
  EXPECT_EQ(this->enters_, 2);
}

// Opening again allocates nothing while the pool is big enough, and a row keeps its position.
TEST_F(Refill, PoolRowsAreReusedByPosition) {
  this->names_ = {"Alpha", "Beta", "Gamma"};
  this->menu_.enter();
  ASSERT_TRUE(this->menu_.back());
  ASSERT_EQ(this->pool_.size(), 3u);
  MenuItem *first = this->submenu_.get_item(0);

  this->names_ = {"Delta"};
  this->menu_.enter();
  ASSERT_TRUE(this->menu_.back());

  EXPECT_EQ(this->pool_.size(), 3u);
  EXPECT_EQ(this->pool_[1]->get_parent(), nullptr);  // the rows it left out are detached
  EXPECT_EQ(this->pool_[2]->get_parent(), nullptr);

  this->names_ = {"Alpha", "Beta", "Gamma"};
  this->menu_.enter();

  EXPECT_EQ(this->pool_.size(), 3u);
  EXPECT_EQ(this->submenu_.get_item(0), first);
  EXPECT_EQ(this->menu_.rows, (std::vector<std::string>{"Alpha", "Beta", "Gamma"}));
}

// HOME or the display-off timer in the middle of an edit: the row has had its on_leave before
// the next open refills the submenu under it.
TEST_F(Refill, AnEditCutShortByHideEndsBeforeTheNextRefill) {
  this->editable_.set_text("Edit me");
  this->editable_.add_on_enter_callback([this]() { this->events_.push_back("edit"); });
  this->editable_.add_on_leave_callback([this]() { this->events_.push_back("done"); });
  this->with_editable_ = true;

  this->menu_.enter();
  this->menu_.enter();  // starts editing the first row
  this->menu_.hide();
  this->menu_.show_main();
  this->menu_.enter();

  EXPECT_EQ(this->events_, (std::vector<std::string>{"open", "edit", "done", "open"}));
  EXPECT_EQ(this->menu_.rows, (std::vector<std::string>{"Edit me", "First"}));
  this->menu_.back();  // not editing, so this leaves
  EXPECT_TRUE(this->menu_.is_at_main());
}

TEST_F(Refill, EveryEnterHasItsLeave) {
  this->menu_.enter();
  ASSERT_TRUE(this->menu_.back());
  this->menu_.enter();
  this->menu_.hide();
  this->menu_.show_main();

  EXPECT_EQ(this->enters_, 2);
  EXPECT_EQ(this->leaves_, 2);
}

}  // namespace esphome::display_menu_base::testing
