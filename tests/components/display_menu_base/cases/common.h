#pragma once

#include <string>
#include <vector>

#include "esphome/components/display_menu_base/display_menu_base.h"
#include "esphome/components/display_menu_base/menu_item.h"

namespace esphome::display_menu_base::testing {

/// The menu the navigation cases drive: it keeps the rows of the last redraw instead of
/// putting them on a display.
class RecordingMenu : public DisplayMenuComponent {
 public:
  RecordingMenu() {
    this->set_rows(8);
    this->set_active(true);
    // Upstream leaves mode_ uninitialised; codegen always sets it, the cases have to.
    this->set_mode(MENU_MODE_JOYSTICK);
  }

  std::vector<std::string> rows;
  int selected_row{-1};

 protected:
  void draw_menu() override {
    this->rows.clear();
    this->selected_row = -1;
    DisplayMenuComponent::draw_menu();
  }

  void draw_item(const MenuItem *item, uint8_t row, bool selected) override {
    this->rows.push_back(item->get_text());
    if (selected)
      this->selected_row = static_cast<int>(row);
  }
};

}  // namespace esphome::display_menu_base::testing
