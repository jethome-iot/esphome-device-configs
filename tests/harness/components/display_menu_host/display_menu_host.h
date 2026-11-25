#pragma once

#include "esphome/components/display_menu_base/display_menu_base.h"

namespace esphome::display_menu_host {

/// A menu that draws nothing, so a test YAML has a display_menu id for the display_menu actions:
/// their generated calls are compiled against it.
class HostMenu : public display_menu_base::DisplayMenuComponent {
 protected:
  void draw_item(const display_menu_base::MenuItem *item, uint8_t row, bool selected) override {}
};

}  // namespace esphome::display_menu_host
