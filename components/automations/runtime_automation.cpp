#include "runtime_automation.h"
#include <cmath>
#include "automation_storage.h"
#include "esphome/components/switch_hold/switch_hold.h"
#include "esphome/core/log.h"
#ifdef USE_CLIMATE_HUB
#include "esphome/components/climate_hub/climate_hub.h"
#endif

namespace esphome::automations {

static const char *const TAG = "automations";

// Same window the core click trigger was given in the previous engine.
static const uint32_t CLICK_MIN_MS = 200;
static const uint32_t CLICK_MAX_MS = 1000;
// Timer ids carry the run sequence in their low 4 bits.
static const uint8_t MAX_RUNS = 8;

static bool fail(std::string *error, const std::string &message) {
  if (error != nullptr)
    *error = message;
  return false;
}

// --- Conditions ---

bool CompiledCondition::check() const {
  switch (this->type) {
    case ConditionType::INPUT:
#ifdef USE_BINARY_SENSOR
      return this->binary_sensor->get_state_default(false) == this->expected;
#else
      return false;
#endif
    case ConditionType::TEMPERATURE: {
#ifdef USE_SENSOR
      const float value = this->sensor->state;
      if (std::isnan(value))
        return false;
      // Same reading as the triggers: above and below are strict, a range includes its ends.
      if (std::isnan(this->min))
        return value < this->max;
      if (std::isnan(this->max))
        return value > this->min;
      return this->min <= value && value <= this->max;
#else
      return false;
#endif
    }
    case ConditionType::AND:
      for (const auto &sub : this->subs) {
        if (!sub.check())
          return false;
      }
      return true;
    case ConditionType::OR:
      for (const auto &sub : this->subs) {
        if (sub.check())
          return true;
      }
      return false;
    case ConditionType::XOR: {
      size_t hits = 0;
      for (const auto &sub : this->subs)
        hits += sub.check() ? 1 : 0;
      return hits == 1;
    }
    default:
      return false;
  }
}

bool compile_condition(const ConditionConfig &config, CompiledCondition &out, std::string *error) {
  out.type = config.type;
  switch (config.type) {
    case ConditionType::INPUT:
      out.binary_sensor = find_binary_sensor(config.sensor_id);
      out.expected = config.state == InputConditionState::TRUE;
      if (out.binary_sensor != nullptr)
        return true;
      ESP_LOGE(TAG, "Condition: binary sensor 0x%08X not found", static_cast<unsigned>(config.sensor_id));
      return fail(error, "input not found");
    case ConditionType::TEMPERATURE:
      out.sensor = find_sensor(config.sensor_id);
      if (out.sensor == nullptr) {
        ESP_LOGE(TAG, "Condition: sensor 0x%08X not found", static_cast<unsigned>(config.sensor_id));
        return fail(error, "sensor not found");
      }
      switch (config.temperature_type) {
        case TypesTemperatureCondition::BELOW:
          out.min = NAN;
          out.max = config.threshold;
          return true;
        case TypesTemperatureCondition::ABOVE:
          out.min = config.threshold;
          out.max = NAN;
          return true;
        case TypesTemperatureCondition::RANGE:
          out.min = config.min_threshold;
          out.max = config.max_threshold;
          return true;
        default:
          ESP_LOGE(TAG, "Condition: temperature needs a 'temperature_type' of below, above or range");
          return fail(error, "temperature_type missing");
      }
    case ConditionType::AND:
    case ConditionType::OR:
    case ConditionType::XOR:
      // Fail closed: a group missing one member would silently change its meaning.
      if (config.sub_conditions.empty())
        return fail(error, "a group without members");
      for (const auto &sub_config : config.sub_conditions) {
        CompiledCondition sub;
        if (!sub_config.is_valid())
          return fail(error, "a member without a type");
        if (!compile_condition(sub_config, sub, error))
          return false;
        out.subs.push_back(std::move(sub));
      }
      return true;
    default:
      return fail(error, "no type");
  }
}

// --- Triggers ---

template<size_t N> static void set_bits(std::bitset<N> &bits, const std::vector<uint8_t> &values) {
  for (uint8_t v : values) {
    if (v < N)
      bits.set(v);
  }
}

bool CompiledTrigger::cron_matches(const ESPTime &time) const {
  return time.is_valid() && this->seconds[time.second] && this->minutes[time.minute] && this->hours[time.hour] &&
         this->days_of_month[time.day_of_month] && this->months[time.month] && this->days_of_week[time.day_of_week];
}

bool compile_trigger(AutomationStorage *engine, const TriggerConfig &config, CompiledTrigger &out, std::string *error) {
  out.source = config.source;
  switch (config.source) {
    case SourceTrigger::INPUT:
      out.binary_sensor = find_binary_sensor(config.params.input.input_id);
      out.input_type = config.params.input.type;
      if (out.binary_sensor == nullptr) {
        ESP_LOGE(TAG, "Trigger: binary sensor 0x%08X not found", static_cast<unsigned>(config.params.input.input_id));
        return fail(error, "input not found");
      }
      return out.input_type != TypesInputTrigger::NONE || fail(error, "no type");
    case SourceTrigger::SWITCH:
      out.sw = find_switch(config.params.switch_trigger.switch_id);
      out.switch_type = config.params.switch_trigger.type;
      if (out.sw == nullptr) {
        ESP_LOGE(TAG, "Trigger: switch 0x%08X not found",
                 static_cast<unsigned>(config.params.switch_trigger.switch_id));
        return fail(error, "switch not found");
      }
      return out.switch_type != TypesSwitchTrigger::NONE || fail(error, "no type");
    case SourceTrigger::TEMPERATURE:
      out.sensor = find_sensor(config.params.temperature.sensor_id);
      out.temperature_type = config.params.temperature.type;
      out.threshold = config.params.temperature.threshold;
      out.min = config.params.temperature.min_threshold;
      out.max = config.params.temperature.max_threshold;
      if (out.sensor == nullptr) {
        ESP_LOGE(TAG, "Trigger: sensor 0x%08X not found", static_cast<unsigned>(config.params.temperature.sensor_id));
        return fail(error, "sensor not found");
      }
      return out.temperature_type != TypesTemperatureTrigger::NONE || fail(error, "no type");
    case SourceTrigger::CRON:
      if (!engine->has_rtc()) {
        ESP_LOGE(TAG, "Trigger: cron needs a time source (time_id)");
        return fail(error, "cron needs a time source");
      }
      set_bits(out.seconds, config.cron_seconds);
      set_bits(out.minutes, config.cron_minutes);
      set_bits(out.hours, config.cron_hours);
      set_bits(out.days_of_month, config.cron_days_of_month);
      set_bits(out.months, config.cron_months);
      set_bits(out.days_of_week, config.cron_days_of_week);
      return true;
    case SourceTrigger::STARTUP:
      return true;
    default:
      return fail(error, "no source");
  }
}

// --- Actions ---

// Fail closed: a rule that would act on a thermostat or a preset the hub does not have is not
// built at all, rather than built to do nothing.
static bool check_climate(const ClimateActionConfig &config, std::string *error) {
  const std::string who = "thermostat \"" + config.climate + "\"";
#ifdef USE_CLIMATE_HUB
  const climate_hub::ClimateConfig *thermostat =
      global_climate_hub != nullptr ? global_climate_hub->store().get(config.climate) : nullptr;
  if (thermostat == nullptr) {
    ESP_LOGE(TAG, "Action: thermostat '%s' not found", config.climate.c_str());
    return fail(error, who + " not found");
  }
  for (const ClimateStep *step : {&config.step, &config.on, &config.off}) {
    if (step->type == TypeClimateAction::SET_PRESET && thermostat->find_preset(step->preset) == nullptr) {
      ESP_LOGE(TAG, "Action: thermostat '%s' has no preset '%s'", config.climate.c_str(), step->preset.c_str());
      return fail(error, who + " has no preset \"" + step->preset + "\"");
    }
  }
  return true;
#else
  ESP_LOGE(TAG, "Action: no thermostats on this device");
  return fail(error, who + " not found");
#endif
}

bool compile_action(const ActionConfig &config, CompiledAction &out, std::string *error) {
  out.source = config.source;
  switch (config.source) {
    case SourceAction::SWITCH:
      out.target = find_switch(config.params.switch_action.switch_id);
      out.type = config.params.switch_action.type;
      out.invert = config.params.switch_action.invert;
      if (out.target == nullptr) {
        ESP_LOGE(TAG, "Action: switch 0x%08X not found", static_cast<unsigned>(config.params.switch_action.switch_id));
        return fail(error, "switch not found");
      }
      return out.type != TypeSwitchAction::NONE || fail(error, "no type");
    case SourceAction::DELAY:
      out.delay_ms = config.params.delay.delay_ms;
      return true;
    case SourceAction::CLIMATE:
      out.climate = config.climate;
      return check_climate(config.climate, error);
    default:
      return fail(error, "no source");
  }
}

static bool compile_actions(const std::vector<ActionConfig> &configs, std::vector<CompiledAction> &out,
                            const char *label, std::string *error) {
  for (size_t i = 0; i < configs.size(); i++) {
    CompiledAction action;
    std::string why;
    if (!compile_action(configs[i], action, &why))
      return fail(error, std::string(label) + " " + std::to_string(i + 1) + ": " + why);
    out.push_back(action);
  }
  return true;
}

bool names_climate(const AutomationConfig &config, const std::string &climate_id) {
  for (const auto *list : {&config.actions, &config.else_actions}) {
    for (const ActionConfig &action : *list) {
      if (action.source == SourceAction::CLIMATE && action.climate.climate == climate_id)
        return true;
    }
  }
  return false;
}

// --- RuntimeAutomation ---

RuntimeAutomation::RuntimeAutomation(AutomationStorage *engine, const AutomationConfig &config)
    : engine_(engine), id_(config.id), name_(config.name), enabled_(config.enabled), mode_(config.mode) {}

RuntimeAutomation::~RuntimeAutomation() { this->stop(); }

std::unique_ptr<RuntimeAutomation> RuntimeAutomation::build(AutomationStorage *engine, const AutomationConfig &config,
                                                            std::string *error) {
  if (config.triggers.empty()) {
    ESP_LOGE(TAG, "Automation '%s' has no triggers", config.name.c_str());
    fail(error, "No triggers");
    return nullptr;
  }
  std::unique_ptr<RuntimeAutomation> automation(new RuntimeAutomation(engine, config));
  for (size_t i = 0; i < config.triggers.size(); i++) {
    CompiledTrigger trigger;
    std::string why;
    if (!compile_trigger(engine, config.triggers[i], trigger, &why)) {
      ESP_LOGE(TAG, "Automation '%s': trigger cannot be built", config.name.c_str());
      fail(error, "Trigger " + std::to_string(i + 1) + ": " + why);
      return nullptr;
    }
    automation->triggers_.push_back(trigger);
  }
  if (config.condition.is_valid()) {
    auto condition = std::make_unique<CompiledCondition>();
    std::string why;
    if (!compile_condition(config.condition, *condition, &why)) {
      ESP_LOGE(TAG, "Automation '%s': condition cannot be built", config.name.c_str());
      fail(error, "Condition: " + why);
      return nullptr;
    }
    automation->condition_ = std::move(condition);
  }
  if (!compile_actions(config.actions, automation->then_, "Action", error) ||
      !compile_actions(config.else_actions, automation->else_, "Else action", error)) {
    ESP_LOGE(TAG, "Automation '%s': action cannot be built", config.name.c_str());
    return nullptr;
  }
  return automation;
}

void RuntimeAutomation::set_enabled(bool enabled) {
  if (!enabled)
    this->stop();
  this->enabled_ = enabled;
}

void RuntimeAutomation::stop() {
  for (const auto &run : this->runs_)
    this->engine_->cancel_delay(this->timer_id_(run->seq));
  this->runs_.clear();
}

void RuntimeAutomation::on_binary_sensor(binary_sensor::BinarySensor *entity, bool state, bool level) {
  for (auto &trigger : this->triggers_) {
    if (trigger.source != SourceTrigger::INPUT || trigger.binary_sensor != entity)
      continue;
    if (level) {
      // No edge to act on; a click in progress is over, its release will not come.
      trigger.pressed = false;
      continue;
    }
    switch (trigger.input_type) {
      case TypesInputTrigger::PRESS:
        if (state)
          this->fire_(true, state);
        break;
      case TypesInputTrigger::RELEASE:
        if (!state)
          this->fire_(true, state);
        break;
      case TypesInputTrigger::STATE_CHANGE:
        this->fire_(true, state);
        break;
      case TypesInputTrigger::CLICK:
        if (state) {
          trigger.pressed = true;
          trigger.press_start = this->engine_->now_ms();
        } else if (trigger.pressed) {
          trigger.pressed = false;
          const uint32_t length = this->engine_->now_ms() - trigger.press_start;
          if (length >= CLICK_MIN_MS && length <= CLICK_MAX_MS)
            this->fire_(false, false);
        }
        break;
      default:
        break;
    }
  }
}

void RuntimeAutomation::on_switch(switch_::Switch *entity, bool state) {
  for (const auto &trigger : this->triggers_) {
    if (trigger.source != SourceTrigger::SWITCH || trigger.sw != entity)
      continue;
    if ((trigger.switch_type == TypesSwitchTrigger::TURN_ON && !state) ||
        (trigger.switch_type == TypesSwitchTrigger::TURN_OFF && state))
      continue;
    this->fire_(true, state);
  }
}

void RuntimeAutomation::on_sensor(sensor::Sensor *entity, float value) {
  if (std::isnan(value))
    return;
  for (auto &trigger : this->triggers_) {
    if (trigger.source != SourceTrigger::TEMPERATURE || trigger.sensor != entity)
      continue;
    bool crossed = false;
    bool rearm = false;
    switch (trigger.temperature_type) {
      case TypesTemperatureTrigger::BELOW:
        crossed = value < trigger.threshold;
        rearm = value >= trigger.threshold;
        break;
      case TypesTemperatureTrigger::ABOVE:
        crossed = value > trigger.threshold;
        rearm = value <= trigger.threshold;
        break;
      case TypesTemperatureTrigger::RANGE:
        crossed = value >= trigger.min && value <= trigger.max;
        rearm = !crossed;
        break;
      default:
        continue;
    }
    if (trigger.armed && crossed) {
      trigger.armed = false;
      this->fire_(false, false);
    } else if (rearm) {
      trigger.armed = true;
    }
  }
}

void RuntimeAutomation::on_time(const ESPTime &time) {
  for (const auto &trigger : this->triggers_) {
    if (trigger.source == SourceTrigger::CRON && trigger.cron_matches(time))
      this->fire_(false, false);
  }
}

void RuntimeAutomation::on_startup() {
  for (const auto &trigger : this->triggers_) {
    if (trigger.source == SourceTrigger::STARTUP)
      this->fire_(false, false);
  }
}

void RuntimeAutomation::fire_(bool has_state, bool state) {
  if (!this->enabled_)
    return;
  if (!this->runs_.empty()) {
    switch (this->mode_) {
      case AutomationMode::SINGLE:
        return;
      case AutomationMode::RESTART:
        this->stop();
        break;
      case AutomationMode::PARALLEL:
        if (this->runs_.size() >= MAX_RUNS)
          return;
        break;
    }
  }
  ESP_LOGD(TAG, "Automation '%s' is triggered", this->name_.c_str());
  auto run = std::make_unique<Run>();
  run->seq = this->free_seq_();
  run->token = ++this->next_token_;
  run->has_state = has_state;
  run->state = state;
  run->branch = &this->then_;
  if (this->condition_ != nullptr && !this->condition_->check())
    run->branch = &this->else_;
  const uint32_t token = run->token;
  this->runs_.push_back(std::move(run));
  this->step_(token);
}

void RuntimeAutomation::step_(uint32_t token, bool scheduled) {
  Run *run = this->find_run_(token);
  while (run != nullptr && run->cursor < run->branch->size()) {
    const CompiledAction &action = (*run->branch)[run->cursor];
    // A thermostat moves on the next loop pass, off the stack of the callback that fired the
    // rule: the hub hears the same sensors, and would be changed halfway through hearing one.
    const bool wait_for_loop = action.source == SourceAction::CLIMATE && !scheduled;
    if (action.source == SourceAction::DELAY || wait_for_loop) {
      if (!wait_for_loop)
        run->cursor++;
      this->engine_->schedule_delay(this->timer_id_(run->seq), wait_for_loop ? 0 : action.delay_ms, [this, token]() {
        this->engine_->drive([this, token]() { this->step_(token, true); });
      });
      return;
    }
    run->cursor++;
    if (action.source == SourceAction::CLIMATE) {
      this->play_climate_(action, *run);
    } else {
      this->play_switch_(action, *run);
    }
    // The switch callback may have restarted or stopped this automation: then this run is gone
    // and whatever replaced it is already being stepped.
    run = this->find_run_(token);
  }
  if (run != nullptr)
    this->finish_run_(token);
}

void RuntimeAutomation::play_switch_(const CompiledAction &action, const Run &run) {
#ifdef USE_SWITCH
  if (action.source != SourceAction::SWITCH)
    return;
  // A running thermostat's relay is the thermostat's: the run goes on without this step.
  const std::string holder = switch_hold::holder(action.target);
  if (!holder.empty()) {
    ESP_LOGI(TAG, "Automation '%s' left '%s' alone: thermostat '%s' drives it", this->name_.c_str(),
             action.target->get_name().c_str(), holder.c_str());
    return;
  }
  switch (action.type) {
    case TypeSwitchAction::TURN_ON:
      action.target->turn_on();
      break;
    case TypeSwitchAction::TURN_OFF:
      action.target->turn_off();
      break;
    case TypeSwitchAction::TOGGLE:
      action.target->toggle();
      break;
    case TypeSwitchAction::FOLLOW:
      if (!run.has_state)
        return;
      if (run.state != action.invert) {
        action.target->turn_on();
      } else {
        action.target->turn_off();
      }
      break;
    default:
      break;
  }
#endif
}

void RuntimeAutomation::play_climate_(const CompiledAction &action, const Run &run) {
#ifdef USE_CLIMATE_HUB
  const ClimateActionConfig &climate = action.climate;
  const ClimateStep *step = &climate.step;
  if (step->type == TypeClimateAction::FOLLOW) {
    if (!run.has_state)
      return;
    step = run.state ? &climate.on : &climate.off;
  }
  climate_hub::ClimateHub *hub = global_climate_hub;
  if (hub == nullptr)
    return;
  climate_hub::Result result;
  switch (step->type) {
    case TypeClimateAction::TURN_ON:
      result = hub->turn_on(climate.climate);
      break;
    case TypeClimateAction::TURN_OFF:
      result = hub->turn_off(climate.climate);
      break;
    case TypeClimateAction::SET_PRESET:
      result = hub->apply_preset(climate.climate, step->preset);
      break;
    case TypeClimateAction::SET_TARGET:
      result = hub->set_setpoint(climate.climate, step->target);
      break;
    default:
      return;
  }
  // The hub dropped it, or its preset, since the rule was built: the rest of the run goes on.
  if (!result.ok)
    ESP_LOGW(TAG, "Automation '%s': thermostat '%s': %s", this->name_.c_str(), climate.climate.c_str(),
             result.error.c_str());
#else
  // Never built without the hub.
  (void) action;
  (void) run;
#endif
}

RuntimeAutomation::Run *RuntimeAutomation::find_run_(uint32_t token) {
  for (const auto &run : this->runs_) {
    if (run->token == token)
      return run.get();
  }
  return nullptr;
}

void RuntimeAutomation::finish_run_(uint32_t token) {
  for (auto it = this->runs_.begin(); it != this->runs_.end(); ++it) {
    if ((*it)->token == token) {
      this->runs_.erase(it);
      return;
    }
  }
}

uint8_t RuntimeAutomation::free_seq_() const {
  for (uint8_t seq = 0; seq < 16; seq++) {
    bool used = false;
    for (const auto &run : this->runs_)
      used = used || run->seq == seq;
    if (!used)
      return seq;
  }
  return 0;
}

}  // namespace esphome::automations
