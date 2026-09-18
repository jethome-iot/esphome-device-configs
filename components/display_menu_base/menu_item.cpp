#include "menu_item.h"

#include <cstdio>

namespace esphome::display_menu_base {

const LogString *menu_item_type_to_string(MenuItemType type) {
  switch (type) {
    case MenuItemType::MENU_ITEM_LABEL:
      return LOG_STR("MENU_ITEM_LABEL");
    case MenuItemType::MENU_ITEM_MENU:
      return LOG_STR("MENU_ITEM_MENU");
    case MenuItemType::MENU_ITEM_BACK:
      return LOG_STR("MENU_ITEM_BACK");
    case MenuItemType::MENU_ITEM_SELECT:
      return LOG_STR("MENU_ITEM_SELECT");
    case MenuItemType::MENU_ITEM_NUMBER:
      return LOG_STR("MENU_ITEM_NUMBER");
    case MenuItemType::MENU_ITEM_SWITCH:
      return LOG_STR("MENU_ITEM_SWITCH");
    case MenuItemType::MENU_ITEM_COMMAND:
      return LOG_STR("MENU_ITEM_COMMAND");
    case MenuItemType::MENU_ITEM_CUSTOM:
      return LOG_STR("MENU_ITEM_CUSTOM");
    default:
      return LOG_STR("UNKNOWN");
  }
}

void MenuItem::on_enter() { this->on_enter_callbacks_.call(); }

void MenuItem::on_leave() { this->on_leave_callbacks_.call(); }

void MenuItem::on_value_() { this->on_value_callbacks_.call(); }

#ifdef USE_SELECT
std::string MenuItemSelect::get_value_text() const {
  std::string result;

  if (this->value_getter_.has_value()) {
    result = this->value_getter_.value()(this);
  } else {
    if (this->select_var_ != nullptr) {
      auto option = this->select_var_->current_option();
      result.assign(option.c_str(), option.size());
    }
  }

  return result;
}

bool MenuItemSelect::select_next() {
  bool changed = false;

  if (this->select_var_ != nullptr) {
    this->select_var_->make_call().select_next(true).perform();
    this->on_value_();
    changed = true;
  }

  return changed;
}

bool MenuItemSelect::select_prev() {
  bool changed = false;

  if (this->select_var_ != nullptr) {
    this->select_var_->make_call().select_previous(true).perform();
    this->on_value_();
    changed = true;
  }

  return changed;
}
#endif  // USE_SELECT

#ifdef USE_NUMBER
std::string MenuItemNumber::get_value_text() const {
  std::string result;

  if (this->value_getter_.has_value()) {
    result = this->value_getter_.value()(this);
  } else {
    char data[32];
    snprintf(data, sizeof(data), this->format_.c_str(), get_number_value_());
    result = data;
  }

  return result;
}

bool MenuItemNumber::select_next() {
  bool changed = false;

  if (this->number_var_ != nullptr) {
    float last = this->number_var_->state;
    this->number_var_->make_call().number_increment(false).perform();

    if (this->number_var_->state != last) {
      this->on_value_();
      changed = true;
    }
  }

  return changed;
}

bool MenuItemNumber::select_prev() {
  bool changed = false;

  if (this->number_var_ != nullptr) {
    float last = this->number_var_->state;
    this->number_var_->make_call().number_decrement(false).perform();

    if (this->number_var_->state != last) {
      this->on_value_();
      changed = true;
    }
  }

  return changed;
}

float MenuItemNumber::get_number_value_() const {
  float result = 0.0;

  if (this->number_var_ != nullptr) {
    if (!this->number_var_->has_state() || this->number_var_->state < this->number_var_->traits.get_min_value()) {
      result = this->number_var_->traits.get_min_value();
    } else if (this->number_var_->state > this->number_var_->traits.get_max_value()) {
      result = this->number_var_->traits.get_max_value();
    } else {
      result = this->number_var_->state;
    }
  }

  return result;
}
#endif  // USE_NUMBER

#ifdef USE_SWITCH
std::string MenuItemSwitch::get_value_text() const {
  std::string result;

  if (this->value_getter_.has_value()) {
    result = this->value_getter_.value()(this);
  } else {
    result = this->get_switch_state_() ? this->switch_on_text_ : this->switch_off_text_;
  }

  return result;
}

bool MenuItemSwitch::select_next() { return this->toggle_switch_(); }

bool MenuItemSwitch::select_prev() { return this->toggle_switch_(); }

bool MenuItemSwitch::get_switch_state_() const { return (this->switch_var_ != nullptr && this->switch_var_->state); }

bool MenuItemSwitch::toggle_switch_() {
  bool changed = false;

  if (this->switch_var_ != nullptr) {
    this->switch_var_->toggle();
    this->on_value_();
    changed = true;
  }

  return changed;
}
#endif  // USE_SWITCH

std::string MenuItemCustom::get_value_text() const {
  return (this->value_getter_.has_value()) ? this->value_getter_.value()(this) : "";
}

bool MenuItemCommand::select_next() {
  this->on_value_();
  return true;
}

bool MenuItemCommand::select_prev() {
  this->on_value_();
  return true;
}

bool MenuItemCustom::select_next() {
  this->on_next_();
  this->on_value_();
  return true;
}

bool MenuItemCustom::select_prev() {
  this->on_prev_();
  this->on_value_();
  return true;
}

void MenuItemCustom::on_next_() { this->on_next_callbacks_.call(); }

void MenuItemCustom::on_prev_() { this->on_prev_callbacks_.call(); }

// JetHome (fit_text): a row label for text the device did not author. As strict as the font's
// own decoder, which stops drawing a row the moment it meets a sequence it refuses.
std::string fit_text(const std::string &text, size_t chars, const std::function<bool(uint32_t)> &can_draw) {
  // The shortest code point each lead length may carry; anything below it is an overlong form.
  static const uint32_t SHORTEST[] = {0, 0, 0x80, 0x800, 0x10000};
  std::string out;
  size_t i = 0;
  while (i < text.size() && chars-- > 0) {
    const auto lead = static_cast<unsigned char>(text[i]);
    size_t len = lead < 0x80             ? 1
                 : (lead & 0xE0) == 0xC0 ? 2
                 : (lead & 0xF0) == 0xE0 ? 3
                 : (lead & 0xF8) == 0xF0 ? 4
                                         : 0;
    // However far the sequence got: a broken one still stands for the bytes it did carry and
    // leaves the byte that broke it to be read on its own, so one `?` is one intended character.
    size_t taken = 1;
    uint32_t code_point = lead;
    if (len > 1) {
      code_point = lead & (0xFF >> (len + 1));
      while (taken < len && i + taken < text.size() && (static_cast<unsigned char>(text[i + taken]) & 0xC0) == 0x80) {
        code_point = (code_point << 6) | (static_cast<unsigned char>(text[i + taken]) & 0x3F);
        taken++;
      }
    }
    const bool valid = len > 0 && taken == len && lead != 0 &&
                       (len == 1 || (code_point >= SHORTEST[len] && code_point <= 0x10FFFF &&
                                     (code_point < 0xD800 || code_point > 0xDFFF)));
    out += valid && can_draw(code_point) ? text.substr(i, len) : "?";
    i += taken;
  }
  return out;
}

}  // namespace esphome::display_menu_base
