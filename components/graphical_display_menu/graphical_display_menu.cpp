#include "esphome/core/defines.h"  // JETHOME: feature flags, see display_menu_base/jethome_features.py
#include "graphical_display_menu.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <cstdlib>
#ifdef JETHOME_GDM_SHRINK_LABEL
#include <cstring>
#endif
#include "esphome/components/display/display.h"

namespace esphome::graphical_display_menu {

static const char *const TAG = "graphical_display_menu";

void GraphicalDisplayMenu::setup() {
  if (this->display_ != nullptr) {
    display::display_writer_t writer = [this](display::Display &it) { this->draw_menu(); };
    this->display_page_ = make_unique<display::DisplayPage>(writer);
  }

  if (!this->menu_item_value_.has_value()) {
    this->menu_item_value_ = [](const MenuItemValueArguments *it) {
#ifdef JETHOME_GDM_VALUE_FORMAT
      std::string label;
      if (it->is_item_selected && it->is_menu_editing) {
        label.append(" >");
        label.append(it->item->get_value_text());
        label.append("<");
      } else if (it->item->get_type() == display_menu_base::MENU_ITEM_SWITCH) {
        label.append(" (");
        label.append(it->item->get_value_text());
        label.append(")");
      } else {
        label.append(": ");
        label.append(it->item->get_value_text());
      }
#else
      std::string label = " ";
      if (it->is_item_selected && it->is_menu_editing) {
        label.append(">");
        label.append(it->item->get_value_text());
        label.append("<");
      } else {
        label.append("(");
        label.append(it->item->get_value_text());
        label.append(")");
      }
#endif
      return label;
    };
  }

  display_menu_base::DisplayMenuComponent::setup();
}

void GraphicalDisplayMenu::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Graphical Display Menu\n"
                "  Has Display: %s\n"
                "  Popup Mode: %s\n"
                "  Advanced Drawing Mode: %s\n"
                "  Has Font: %s\n"
                "  Mode: %s\n"
                "  Active: %s\n"
                "  Menu items:",
                YESNO(this->display_ != nullptr), YESNO(this->display_ != nullptr), YESNO(this->display_ == nullptr),
                YESNO(this->font_ != nullptr),
                this->mode_ == display_menu_base::MENU_MODE_ROTARY ? "Rotary" : "Joystick", YESNO(this->active_));
  for (size_t i = 0; i < this->displayed_item_->items_size(); i++) {
    auto *item = this->displayed_item_->get_item(i);
    ESP_LOGCONFIG(TAG, "  %i: %s (Type: %s, Immediate Edit: %s)", i, item->get_text().c_str(),
                  LOG_STR_ARG(display_menu_base::menu_item_type_to_string(item->get_type())),
                  YESNO(item->get_immediate_edit()));
  }
}

void GraphicalDisplayMenu::set_display(display::Display *display) { this->display_ = display; }

void GraphicalDisplayMenu::set_font(display::BaseFont *font) { this->font_ = font; }

void GraphicalDisplayMenu::set_foreground_color(Color foreground_color) { this->foreground_color_ = foreground_color; }
void GraphicalDisplayMenu::set_background_color(Color background_color) { this->background_color_ = background_color; }
#ifdef JETHOME_GDM_FILL_ROW
void GraphicalDisplayMenu::set_fill_row(bool val) { this->fill_row_ = val; }
#endif
#ifdef JETHOME_GDM_RESTORE_PAGE
void GraphicalDisplayMenu::set_restore_page(bool val) { this->restore_page_ = val; }
#endif
#ifdef JETHOME_GDM_SHRINK_LABEL
void GraphicalDisplayMenu::set_shrink_label(bool val) { this->shrink_label_ = val; }
#endif

void GraphicalDisplayMenu::on_before_show() {
  if (this->display_ != nullptr) {
    this->previous_display_page_ = this->display_->get_active_page();
    this->display_->show_page(this->display_page_.get());
    this->display_->clear();
  } else {
    this->update();
  }
}

#ifdef JETHOME_GDM_SHRINK_LABEL
std::string GraphicalDisplayMenu::shrink_text_to_width_(display::Display *display, const std::string &str,
                                                        int max_width) {
  static const char DOTS_STR[] = "…";
  const size_t str_size = str.size();

  if (str_size < 4 || max_width <= 0)
    return str;

  int x1, y1, width, height;
  display->get_text_bounds(0, 0, str.c_str(), this->font_, display::TextAlign::TOP_LEFT, &x1, &y1, &width, &height);
  if (width <= max_width)
    return str;

  const size_t buffer_size = str_size + sizeof(DOTS_STR) + 1;
  std::unique_ptr<char[]> buffer(new char[buffer_size]{0});

  size_t left_end = str_size / 2;
  size_t right_start = left_end + 1;
  bool shrink_left = true;

  // Widen the cut one character at a time, alternating sides, until it fits or nothing is left to cut
  while (width > max_width && (left_end > 0 || right_start < str_size)) {
    memcpy(buffer.get(), str.c_str(), left_end);
    strlcpy(buffer.get() + left_end, DOTS_STR, buffer_size - left_end);
    strlcat(buffer.get(), str.c_str() + right_start, buffer_size);

    display->get_text_bounds(0, 0, buffer.get(), this->font_, display::TextAlign::TOP_LEFT, &x1, &y1, &width,
                             &height);

    if (shrink_left && left_end > 0) {
      left_end--;
    } else if (right_start < str_size) {
      right_start++;
    } else {
      left_end--;
    }
    shrink_left = !shrink_left;
  }

  return std::string(buffer.get());
}
#endif  // JETHOME_GDM_SHRINK_LABEL

void GraphicalDisplayMenu::on_before_hide() {
#ifdef JETHOME_GDM_RESTORE_PAGE
  if (this->restore_page_ && this->previous_display_page_ != nullptr) {
#else
  if (this->previous_display_page_ != nullptr) {
#endif
    this->display_->show_page((display::DisplayPage *) this->previous_display_page_);
    this->display_->clear();
    this->update();
    this->previous_display_page_ = nullptr;
  } else {
    this->update();
  }
}

void GraphicalDisplayMenu::draw_and_update() {
  this->update();

  // If we're in advanced drawing mode we won't have a display and will instead require the update callback to do
  // our drawing
  if (this->display_ != nullptr) {
    draw_menu();
  }
}

void GraphicalDisplayMenu::draw_menu() {
  if (this->display_ == nullptr) {
    ESP_LOGE(TAG, "draw_menu() called without a display_. This is only available when using the menu in pop up mode");
    return;
  }
  display::Rect bounds(0, 0, this->display_->get_width(), this->display_->get_height());
  this->draw_menu_internal_(this->display_, &bounds);
}

void GraphicalDisplayMenu::draw(display::Display *display, const display::Rect *bounds) {
  this->draw_menu_internal_(display, bounds);
}

void GraphicalDisplayMenu::draw_menu_internal_(display::Display *display, const display::Rect *bounds) {
  int16_t total_height = 0;
  int16_t max_width = 0;
  int y_padding = 2;
  bool scroll_menu_items = false;
  std::vector<display::Rect> menu_dimensions;
  int number_items_fit_to_screen = 0;
  const int max_item_index = this->displayed_item_->items_size() - 1;

  for (size_t i = 0; max_item_index >= 0 && i <= static_cast<size_t>(max_item_index); i++) {
    const auto *item = this->displayed_item_->get_item(i);
    const bool selected = i == this->cursor_index_;
    const display::Rect item_dimensions = this->measure_item_(display, item, bounds, selected);

    menu_dimensions.push_back(item_dimensions);
    total_height += item_dimensions.h + (i == 0 ? 0 : y_padding);
    max_width = std::max(max_width, item_dimensions.w);

    if (total_height <= bounds->h) {
      number_items_fit_to_screen++;
    } else {
      // Scroll the display if the selected item or the item immediately after it overflows
      if ((selected) || (i == this->cursor_index_ + 1)) {
        scroll_menu_items = true;
      }
    }
  }

  // Determine what items to draw
  int first_item_index = 0;
  int last_item_index = max_item_index;

  if (number_items_fit_to_screen <= 1) {
    // If only one item can fit to the bounds draw the current cursor item
    last_item_index = std::min(last_item_index, this->cursor_index_ + 1);
    first_item_index = this->cursor_index_;
  } else {
    if (scroll_menu_items) {
      // Attempt to draw the item after the current item (+1 for equality check in the draw loop)
      last_item_index = std::min(last_item_index, this->cursor_index_ + 1);

      // Go back through the measurements to determine how many prior items we can fit
      int height_left_to_use = bounds->h;
      for (int i = last_item_index; i >= 0; i--) {
        const display::Rect item_dimensions = menu_dimensions[i];
        height_left_to_use -= (item_dimensions.h + y_padding);

        if (height_left_to_use <= 0) {
          // Ran out of space -  this is our first item to draw
          first_item_index = i;
          break;
        }
      }
      const int items_to_draw = last_item_index - first_item_index;
      // Dont't draw last item partially if it is the selected item
      if ((this->cursor_index_ == last_item_index) && (number_items_fit_to_screen <= items_to_draw) &&
          (first_item_index < max_item_index)) {
        first_item_index++;
      }
    }
  }

  // Render the items into the view port
  display->start_clipping(*bounds);

  display->filled_rectangle(bounds->x, bounds->y, max_width, total_height, this->background_color_);
  auto y_offset = bounds->y;
  for (size_t i = static_cast<size_t>(first_item_index);
       last_item_index >= 0 && i <= static_cast<size_t>(last_item_index); i++) {
    const auto *item = this->displayed_item_->get_item(i);
    const bool selected = i == this->cursor_index_;
    display::Rect dimensions = menu_dimensions[i];

    dimensions.y = y_offset;
    dimensions.x = bounds->x;
    this->draw_item_(display, item, &dimensions, selected);

    y_offset += dimensions.h + y_padding;
  }

  display->end_clipping();
}

display::Rect GraphicalDisplayMenu::measure_item_(display::Display *display, const display_menu_base::MenuItem *item,
                                                  const display::Rect *bounds, const bool selected) {
  display::Rect dimensions(0, 0, 0, 0);

  if (selected) {
    // TODO: Support selection glyph
    dimensions.w += 0;
    dimensions.h += 0;
  }

  std::string label = item->get_text();
  if (item->has_value()) {
    // Append to label
    MenuItemValueArguments args(item, selected, this->editing_);
    label.append(this->menu_item_value_.value(&args));
  }

  int x1;
  int y1;
  int width;
  int height;
  display->get_text_bounds(0, 0, label.c_str(), this->font_, display::TextAlign::TOP_LEFT, &x1, &y1, &width, &height);

#ifdef JETHOME_GDM_FILL_ROW
  dimensions.w = this->fill_row_ ? bounds->w : std::min((int16_t) width, bounds->w);
#else
  dimensions.w = std::min((int16_t) width, bounds->w);
#endif
  dimensions.h = std::min((int16_t) height, bounds->h);

  return dimensions;
}

inline void GraphicalDisplayMenu::draw_item_(display::Display *display, const display_menu_base::MenuItem *item,
                                             const display::Rect *bounds, const bool selected) {
  const auto background_color = selected ? this->foreground_color_ : this->background_color_;
  const auto foreground_color = selected ? this->background_color_ : this->foreground_color_;

  // int background_width = std::max(bounds->width, available_width);
  int background_width = bounds->w;

  display->filled_rectangle(bounds->x, bounds->y, background_width, bounds->h, background_color);

  std::string label = item->get_text();
#ifdef JETHOME_GDM_SHRINK_LABEL
  // The value has to be produced before the label so the label can be shrunk to whatever width the
  // value leaves over; upstream simply appends it (see #else).
  std::string value;
  if (item->has_value()) {
    MenuItemValueArguments args(item, selected, this->editing_);
    value = this->menu_item_value_.value(&args);
  }

  if (this->shrink_label_) {
    int label_width = background_width;
    if (!value.empty()) {
      int x1, y1, width, height;
      display->get_text_bounds(0, 0, value.c_str(), this->font_, display::TextAlign::TOP_LEFT, &x1, &y1, &width,
                               &height);
      label_width -= width;
    }
    label = this->shrink_text_to_width_(display, label, label_width);
  }
  label.append(value);
#else
  if (item->has_value()) {
    MenuItemValueArguments args(item, selected, this->editing_);
    label.append(this->menu_item_value_.value(&args));
  }
#endif

  display->print(bounds->x, bounds->y, this->font_, foreground_color, display::TextAlign::TOP_LEFT, label.c_str(),
                 background_color);
}

void GraphicalDisplayMenu::draw_item(const display_menu_base::MenuItem *item, const uint8_t row, const bool selected) {
  ESP_LOGE(TAG, "draw_item(MenuItem *item, uint8_t row, bool selected) called. The graphical_display_menu specific "
                "draw_item should be called.");
}

void GraphicalDisplayMenu::update() { this->on_redraw_callbacks_.call(); }

}  // namespace esphome::graphical_display_menu
