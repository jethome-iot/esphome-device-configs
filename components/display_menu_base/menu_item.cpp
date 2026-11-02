#include "menu_item.h"

#include <algorithm>  // JetHome: apply on leave
#include <cmath>      // JetHome: apply on leave
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
  } else if (this->pending_.has_value() && this->select_var_->has_index(*this->pending_)) {  // JetHome: apply on leave
    result = this->select_var_->option_at(*this->pending_);
  } else {
    if (this->select_var_ != nullptr) {
      auto option = this->select_var_->current_option();
      result.assign(option.c_str(), option.size());
    }
  }

  return result;
}

bool MenuItemSelect::select_next() {
  // JetHome: apply on leave
  if (this->editing_)
    return this->step_pending_(true);

  bool changed = false;

  if (this->select_var_ != nullptr) {
    this->select_var_->make_call().select_next(true).perform();
    this->on_value_();
    changed = true;
  }

  return changed;
}

bool MenuItemSelect::select_prev() {
  // JetHome: apply on leave
  if (this->editing_)
    return this->step_pending_(false);

  bool changed = false;

  if (this->select_var_ != nullptr) {
    this->select_var_->make_call().select_previous(true).perform();
    this->on_value_();
    changed = true;
  }

  return changed;
}

// JetHome: apply on leave
void MenuItemSelect::on_enter() {
  this->editing_ = true;
  this->pending_.reset();
  MenuItem::on_enter();
}

void MenuItemSelect::on_leave() {
  const optional<size_t> picked = this->pending_;
  this->editing_ = false;
  this->pending_.reset();
  if (picked.has_value() && this->select_var_->has_index(*picked)) {
    const optional<size_t> active = this->select_var_->active_index();
    if (!active.has_value() || *active != *picked) {
      this->select_var_->make_call().set_index(*picked).perform();
      this->on_value_();
    }
  }
  MenuItem::on_leave();
}

bool MenuItemSelect::step_pending_(bool next) {
  const size_t size = this->select_var_ != nullptr ? this->select_var_->size() : 0;
  if (size == 0)
    return false;
  // The first step starts from the select's option, so a change made elsewhere while the row
  // was open is not written back; with no option it lands on an end, as SelectCall's does.
  const optional<size_t> from = this->pending_.has_value() ? this->pending_ : this->select_var_->active_index();
  if (!from.has_value()) {
    this->pending_ = next ? 0 : size - 1;
  } else {
    this->pending_ = (*from + (next ? 1 : size - 1)) % size;
  }
  return true;
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
  // JetHome: apply on leave
  if (this->editing_)
    return this->step_pending_(true);

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
  // JetHome: apply on leave
  if (this->editing_)
    return this->step_pending_(false);

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
    // JetHome: apply on leave. An open edit shows the value being picked.
    const float value = this->pending_.value_or(this->number_var_->state);
    if (!this->number_var_->has_state() || value < this->number_var_->traits.get_min_value()) {
      result = this->number_var_->traits.get_min_value();
    } else if (value > this->number_var_->traits.get_max_value()) {
      result = this->number_var_->traits.get_max_value();
    } else {
      result = value;
    }
  }

  return result;
}

// JetHome: apply on leave
void MenuItemNumber::on_enter() {
  // The rotary mode opens an edit on an immediate_edit number too; it still applies every step.
  this->editing_ = !this->immediate_edit_;
  this->pending_.reset();
  MenuItem::on_enter();
}

void MenuItemNumber::on_leave() {
  const optional<float> picked = this->pending_;
  this->editing_ = false;
  this->pending_.reset();
  if (picked.has_value() && *picked != this->number_var_->state) {
    this->number_var_->make_call().set_value(*picked).perform();
    this->on_value_();
  }
  MenuItem::on_leave();
}

bool MenuItemNumber::step_pending_(bool up) {
  if (this->number_var_ == nullptr)
    return false;
  const auto &traits = this->number_var_->traits;
  // The first step starts from the state the row shows, as for a select; with none there is
  // nothing to step from, and NumberCall refuses the same.
  if (!this->pending_.has_value()) {
    if (!this->number_var_->has_state())
      return false;
    this->origin_ = std::clamp(this->number_var_->state, traits.get_min_value(), traits.get_max_value());
    this->steps_ = 0;
  }
  // Counted steps, not a running sum, so stepping back lands on the origin exactly.
  const int steps = this->steps_ + (up ? 1 : -1);
  const float step = std::isnan(traits.get_step()) ? 1 : traits.get_step();
  const float raw = this->origin_ + steps * step;
  const float value = std::clamp(raw, traits.get_min_value(), traits.get_max_value());
  if (value == this->pending_.value_or(this->origin_))
    return false;
  // A step cut at a limit counts from that limit, so the way back is a whole step from it.
  if (value != raw) {
    this->origin_ = value;
    this->steps_ = 0;
  } else {
    this->steps_ = steps;
  }
  this->pending_ = value;
  return true;
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

}  // namespace esphome::display_menu_base
