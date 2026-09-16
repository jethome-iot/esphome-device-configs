#pragma once

#include <bitset>
#include <memory>
#include <string>
#include <vector>
#include "automation_config.h"
#include "entity_lookup.h"
#include "esphome/core/time.h"

namespace esphome::automations {

class AutomationStorage;

struct CompiledCondition {
  ConditionType type{ConditionType::NONE};
  binary_sensor::BinarySensor *binary_sensor{nullptr};
  bool expected{true};
  sensor::Sensor *sensor{nullptr};
  float min{NAN};
  float max{NAN};
  std::vector<CompiledCondition> subs;

  bool check() const;
};

struct CompiledAction {
  SourceAction source{SourceAction::NONE};
  switch_::Switch *target{nullptr};
  TypeSwitchAction type{TypeSwitchAction::NONE};
  bool invert{false};
  uint32_t delay_ms{0};
};

struct CompiledTrigger {
  SourceTrigger source{SourceTrigger::NONE};
  binary_sensor::BinarySensor *binary_sensor{nullptr};
  TypesInputTrigger input_type{TypesInputTrigger::NONE};
  switch_::Switch *sw{nullptr};
  TypesSwitchTrigger switch_type{TypesSwitchTrigger::NONE};
  sensor::Sensor *sensor{nullptr};
  TypesTemperatureTrigger temperature_type{TypesTemperatureTrigger::NONE};
  float threshold{0};
  float min{0};
  float max{0};
  std::bitset<61> seconds;
  std::bitset<60> minutes;
  std::bitset<24> hours;
  std::bitset<32> days_of_month;
  std::bitset<13> months;
  std::bitset<8> days_of_week;
  // Edge detection: the value was on the far side of the threshold (or outside the range).
  bool armed{true};
  bool pressed{false};
  uint32_t press_start{0};

  bool cron_matches(const ESPTime &time) const;
};

// Resolve one config item against the registered entities. Free functions so the unit tests
// can build and inspect them without a rule around them.
bool compile_trigger(AutomationStorage *engine, const TriggerConfig &config, CompiledTrigger &out);
bool compile_condition(const ConditionConfig &config, CompiledCondition &out);
bool compile_action(const ActionConfig &config, CompiledAction &out);

// One rule, built from its config: matches events, gates by mode, runs the action list.
class RuntimeAutomation {
 public:
  // nullptr when an entity is missing or the config cannot run as written.
  static std::unique_ptr<RuntimeAutomation> build(AutomationStorage *engine, const AutomationConfig &config);
  ~RuntimeAutomation();

  uint32_t get_id() const { return this->id_; }
  const std::string &get_name() const { return this->name_; }
  bool is_enabled() const { return this->enabled_; }
  void set_enabled(bool enabled);
  bool is_running() const { return !this->runs_.empty(); }
  void stop();

  void on_binary_sensor(binary_sensor::BinarySensor *entity, bool state);
  void on_switch(switch_::Switch *entity, bool state);
  void on_sensor(sensor::Sensor *entity, float value);
  void on_time(const ESPTime &time);
  void on_startup();

  const std::vector<CompiledTrigger> &get_triggers() const { return this->triggers_; }

 protected:
  struct Run {
    uint8_t seq;
    size_t cursor{0};
    const std::vector<CompiledAction> *branch;
    bool has_state{false};
    bool state{false};
  };

  RuntimeAutomation(AutomationStorage *engine, const AutomationConfig &config);

  void fire_(bool has_state, bool state);
  void step_(uint8_t seq);
  Run *find_run_(uint8_t seq);
  void finish_run_(uint8_t seq);
  uint8_t free_seq_() const;
  uint32_t timer_id_(uint8_t seq) const { return (this->id_ << 4) | seq; }
  void play_switch_(const CompiledAction &action, const Run &run);

  AutomationStorage *engine_;
  uint32_t id_;
  std::string name_;
  bool enabled_;
  AutomationMode mode_;
  std::vector<CompiledTrigger> triggers_;
  std::unique_ptr<CompiledCondition> condition_;
  std::vector<CompiledAction> then_;
  std::vector<CompiledAction> else_;
  std::vector<std::unique_ptr<Run>> runs_;
};

}  // namespace esphome::automations
