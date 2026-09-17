#include <gtest/gtest.h>

#include "common.h"

namespace esphome::display_menu_base::testing {

// The Relay N / Input N rows are built at boot with a text lambda that reads the entity, so the
// text has to be re-read on every draw: one taken at add_item() would show a stale state for
// good, and nothing about that would fail a build.
TEST(RowText, ARowFollowsWhatItsOwnLambdaReads) {
  bool first_on = false;
  bool second_on = false;

  MenuItemMenu root;
  MenuItem first{MENU_ITEM_LABEL};
  first.set_text([&first_on](const MenuItem *) { return std::string(first_on ? "Relay 1 (On)" : "Relay 1 (Off)"); });
  MenuItem second{MENU_ITEM_LABEL};
  second.set_text([&second_on](const MenuItem *) { return std::string(second_on ? "Relay 2 (On)" : "Relay 2 (Off)"); });
  root.add_item(&first);
  root.add_item(&second);

  RecordingMenu menu;
  menu.set_root_item(&root);
  menu.draw();
  EXPECT_EQ(menu.rows, (std::vector<std::string>{"Relay 1 (Off)", "Relay 2 (Off)"}));

  second_on = true;
  menu.draw();
  EXPECT_EQ(menu.rows, (std::vector<std::string>{"Relay 1 (Off)", "Relay 2 (On)"}));
}

}  // namespace esphome::display_menu_base::testing
