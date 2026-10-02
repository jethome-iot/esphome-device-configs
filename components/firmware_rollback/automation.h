#pragma once

#include <string>
#include "esphome/core/application.h"
#include "esphome/core/automation.h"
#include "firmware_rollback.h"

namespace esphome::firmware_rollback {

template<typename... Ts> class RefreshAction final : public Action<Ts...>, public Parented<FirmwareRollback> {
 public:
  void play(const Ts &...x) override { this->parent_->refresh(); }
};

// Reboots on success, so the actions after it only run when it failed.
template<typename... Ts> class RollbackAction final : public Action<Ts...>, public Parented<FirmwareRollback> {
 public:
  Trigger<std::string> *get_error_trigger() { return &this->error_trigger_; }

  void play(const Ts &...x) override {
    const char *error = this->parent_->rollback();
    if (error == nullptr) {
      App.safe_reboot();
      return;
    }
    this->error_trigger_.trigger(std::string(error));
  }

 protected:
  Trigger<std::string> error_trigger_;
};

template<typename... Ts> class IsAvailableCondition final : public Condition<Ts...>, public Parented<FirmwareRollback> {
 public:
  bool check(const Ts &...x) override { return this->parent_->available(); }
};

}  // namespace esphome::firmware_rollback
