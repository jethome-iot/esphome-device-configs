#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "automation_config.h"
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "esphome/core/component.h"
#include "esphome/core/optional.h"
#include "esphome/core/time.h"
#include "runtime_automation.h"

namespace esphome {

namespace time {
class RealTimeClock;
}

namespace automations {

// Loads the rules from <storage>/<folder>/*.json, runs them, and edits them at run time.
// The mutators may be called from any task: they execute on the loop task and block the caller.
class AutomationStorage : public Component {
 public:
  AutomationStorage();

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1; }

  /// Returns the assigned id, 0 on failure. The config's own id is ignored.
  uint32_t add_automation(const AutomationConfig &config);
  bool update_automation(uint32_t id, const AutomationConfig &new_config);
  bool remove_automation(uint32_t id);
  /// False when no rule carries that id. `persisted` reports whether the change also reached
  /// flash: it can fail on its own, leaving the rule live but back as it was after a reboot.
  bool set_enable_automation(uint32_t id, bool enable, bool *persisted = nullptr);
  void reset_all();
  /// Names collide by sanitized filename; `exclude_id` never blocks itself.
  bool is_name_taken(const std::string &name, uint32_t exclude_id = 0) const;

  AutomationConfigStorage &configs() { return this->config_storage_; }

  void set_time_source(time::RealTimeClock *rtc) { this->rtc_ = rtc; }
  void set_folder_path(const std::string &path) { this->folder_path_ = path; }
  void set_storage(filesystem_storage_abstract::FilesystemStorageAbstract *storage) {
    this->storage_backend_ = storage;
  }

  bool has_rtc() const { return this->rtc_ != nullptr; }
  // Virtual so the unit tests can hold a delay and fire it themselves.
  virtual void schedule_delay(uint32_t id, uint32_t delay_ms, std::function<void()> &&f) {
    this->set_timeout(id, delay_ms, std::move(f));
  }
  virtual void cancel_delay(uint32_t id) { this->cancel_timeout(id); }

  template<typename E> struct Subscription {
    AutomationStorage *engine;
    E *entity;
  };
  void dispatch_binary_sensor_(binary_sensor::BinarySensor *entity, bool state);
  void dispatch_switch_(switch_::Switch *entity, bool state);
  void dispatch_sensor_(sensor::Sensor *entity, float value);

 protected:
  uint32_t add_automation_(const AutomationConfig &config);
  bool update_automation_(uint32_t id, const AutomationConfig &new_config);
  bool remove_automation_(uint32_t id);
  bool set_enable_automation_(uint32_t id, bool enable, bool *persisted);
  void reset_all_();
  bool run_on_loop_(std::function<bool()> &&job);

  void subscribe_(const RuntimeAutomation &automation);
  void check_time_();

  std::string sanitize_filename_(const std::string &name) const;
  std::string get_folder_path_() const;
  std::string get_filepath_for_name_(const std::string &name) const;
  uint32_t allocate_id_();
  bool ensure_directory_exists_(const std::string &path);
  bool load_automation_from_file_(const std::string &filepath);
  bool save_automation_to_file_(const AutomationConfig &config);
  bool delete_automation_file_(const std::string &name);
  std::vector<bool> resolve_duplicates_();
  void normalize_filenames_(const std::vector<std::string> &filenames, const std::vector<bool> &changed);
  int find_automation_index_by_id_(uint32_t id);

  void print_trigger_info_(const TriggerConfig &trigger, int indent);
  void print_condition_info_(const ConditionConfig &condition, int indent);
  void print_action_info_(const ActionConfig &action, int indent);

  // Index-aligned with config_storage_; a config that failed to build keeps a nullptr slot.
  std::vector<std::unique_ptr<RuntimeAutomation>> automations_;
  AutomationConfigStorage config_storage_;
  uint32_t next_id_{1};
  time::RealTimeClock *rtc_{nullptr};
  optional<ESPTime> last_check_;
  void *loop_task_{nullptr};

  // One subscription per entity, kept for the life of the device.
  std::vector<std::unique_ptr<Subscription<binary_sensor::BinarySensor>>> binary_sensor_subs_;
  std::vector<std::unique_ptr<Subscription<switch_::Switch>>> switch_subs_;
  std::vector<std::unique_ptr<Subscription<sensor::Sensor>>> sensor_subs_;

  filesystem_storage_abstract::FilesystemStorageAbstract *storage_backend_{nullptr};
  std::string folder_path_{"automations"};
};

}  // namespace automations

extern automations::AutomationStorage *global_automation_storage;  // NOLINT

}  // namespace esphome
