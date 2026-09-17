#include "automation_storage.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace esphome {

automations::AutomationStorage *global_automation_storage = nullptr;  // NOLINT

namespace automations {

static const char *const TAG = "automations";

// AutomationConfigStorage indexes by uint8_t.
static const uint32_t MAX_AUTOMATIONS = 255;
// LittleFS refuses names longer than CONFIG_LITTLEFS_OBJ_NAME_LEN (64).
static const size_t MAX_FILENAME_BYTES = 48;
// The loader refuses anything larger, so nothing larger may ever be written.
static const size_t MAX_FILE_BYTES = 16384;
// A cron tick further away than this from the previous one is a clock jump, not elapsed time.
static const time_t MAX_TIMESTAMP_DRIFT = 900;
static const uint32_t LOOP_JOB_TIMEOUT_MS = 5000;

// Cut to at most `limit` bytes without splitting a UTF-8 character.
static std::string truncate_utf8(const std::string &text, size_t limit) {
  if (text.size() <= limit)
    return text;
  size_t cut = 0;
  for (size_t i = 0; i < text.size();) {
    auto lead = static_cast<unsigned char>(text[i]);
    size_t width = 1;
    if ((lead & 0xE0) == 0xC0) {
      width = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      width = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      width = 4;
    }
    if (i + width > limit)
      break;
    i += width;
    cut = i;
  }
  return text.substr(0, cut);
}

static bool file_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

AutomationStorage::AutomationStorage() { global_automation_storage = this; }

uint32_t AutomationStorage::now_ms() const { return millis(); }

ESPTime AutomationStorage::clock_now_() { return this->rtc_->now(); }

// --- Setup and runtime ---

// Drives every built rule, and marks the engine as dispatching for as long as it does.
template<typename F>
static void for_each_rule(uint8_t &depth, std::vector<std::unique_ptr<RuntimeAutomation>> &rules, F call) {
  depth++;
  for (size_t i = 0; i < rules.size(); i++) {
    if (rules[i] != nullptr)
      call(*rules[i]);
  }
  depth--;
}

void AutomationStorage::setup() {
#ifdef USE_ESP32
  this->loop_task_ = xTaskGetCurrentTaskHandle();
#endif
  if (this->storage_backend_ == nullptr || !this->storage_backend_->is_mounted()) {
    ESP_LOGE(TAG, "Storage not mounted");
    this->mark_failed();
    return;
  }

  std::string folder_path = this->get_folder_path_();
  if (!this->ensure_directory_exists_(folder_path)) {
    ESP_LOGE(TAG, "Cannot create '%s'", folder_path.c_str());
    this->mark_failed();
    return;
  }

  DIR *dir = opendir(folder_path.c_str());
  if (dir == nullptr) {
    ESP_LOGE(TAG, "Cannot read '%s'", folder_path.c_str());
    this->mark_failed();
    return;
  }
  {
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      std::string filename = entry->d_name;
      if (filename.length() <= 5 || filename.substr(filename.length() - 5) != ".json")
        continue;
      if (this->config_storage_.size() >= MAX_AUTOMATIONS) {
        ESP_LOGE(TAG, "More than %u automations in '%s'; ignoring '%s' and any after it",
                 static_cast<unsigned>(MAX_AUTOMATIONS), folder_path.c_str(), filename.c_str());
        break;
      }
      // setup() runs before the loop feeds the watchdog.
      App.feed_wdt();
      this->load_automation_from_file_(folder_path + "/" + filename);
    }
    closedir(dir);
  }

  this->normalize_filenames_(this->resolve_duplicates_());
  this->config_storage_.sort_by_id();

  for (const auto &config : this->config_storage_.get_all_configs()) {
    auto automation = RuntimeAutomation::build(this, config);
    if (automation != nullptr)
      this->subscribe_(*automation);
    this->automations_.push_back(std::move(automation));
  }
  ESP_LOGD(TAG, "Loaded %u automations from '%s' (next_id: %u)", static_cast<unsigned>(this->config_storage_.size()),
           folder_path.c_str(), static_cast<unsigned>(this->next_id_));

  if (this->rtc_ != nullptr) {
    this->set_interval(1000, [this]() { this->check_time_(); });
  } else {
    ESP_LOGW(TAG, "No time source: cron triggers are disabled");
  }
  // Startup triggers fire once every component has finished setting up.
  this->defer([this]() {
    for_each_rule(this->dispatching_, this->automations_, [](RuntimeAutomation &rule) { rule.on_startup(); });
  });
}

template<typename E, typename F>
static void ensure_subscription(std::vector<std::unique_ptr<AutomationStorage::Subscription<E>>> &subs,
                                AutomationStorage *engine, E *entity, F attach) {
  for (const auto &sub : subs) {
    if (sub->entity == entity)
      return;
  }
  auto sub = std::make_unique<AutomationStorage::Subscription<E>>();
  sub->engine = engine;
  sub->entity = entity;
  attach(sub.get());
  subs.push_back(std::move(sub));
}

void AutomationStorage::subscribe_(const RuntimeAutomation &automation) {
  for (const auto &trigger : automation.get_triggers()) {
    switch (trigger.source) {
#ifdef USE_BINARY_SENSOR
      case SourceTrigger::INPUT:
        ensure_subscription(this->binary_sensor_subs_, this, trigger.binary_sensor,
                            [](Subscription<binary_sensor::BinarySensor> *sub) {
                              sub->entity->add_on_state_callback([sub](bool state) {
                                const bool level = sub->level_only;
                                sub->level_only = false;
                                sub->engine->dispatch_binary_sensor_(sub->entity, state, level);
                              });
                            });
        break;
#endif
#ifdef USE_SWITCH
      case SourceTrigger::SWITCH:
        ensure_subscription(this->switch_subs_, this, trigger.sw, [](Subscription<switch_::Switch> *sub) {
          sub->entity->add_on_state_callback([sub](bool state) { sub->engine->dispatch_switch_(sub->entity, state); });
        });
        break;
#endif
#ifdef USE_SENSOR
      case SourceTrigger::TEMPERATURE:
        ensure_subscription(this->sensor_subs_, this, trigger.sensor, [](Subscription<sensor::Sensor> *sub) {
          sub->entity->add_on_state_callback([sub](float value) { sub->engine->dispatch_sensor_(sub->entity, value); });
        });
        break;
#endif
      default:
        break;
    }
  }
}

void AutomationStorage::expect_level(binary_sensor::BinarySensor *entity) {
  for (const auto &sub : this->binary_sensor_subs_) {
    if (sub->entity == entity)
      sub->level_only = true;
  }
}

void AutomationStorage::dispatch_binary_sensor_(binary_sensor::BinarySensor *entity, bool state, bool level) {
  for_each_rule(this->dispatching_, this->automations_,
                [=](RuntimeAutomation &rule) { rule.on_binary_sensor(entity, state, level); });
}

void AutomationStorage::dispatch_switch_(switch_::Switch *entity, bool state) {
  for_each_rule(this->dispatching_, this->automations_,
                [=](RuntimeAutomation &rule) { rule.on_switch(entity, state); });
}

void AutomationStorage::dispatch_sensor_(sensor::Sensor *entity, float value) {
  for_each_rule(this->dispatching_, this->automations_,
                [=](RuntimeAutomation &rule) { rule.on_sensor(entity, value); });
}

// Same catch-up and clock-jump handling as the core cron trigger, for all rules at once.
void AutomationStorage::check_time_() {
  ESPTime now = this->clock_now_();
  if (!now.is_valid())
    return;
  auto fire = [this](const ESPTime &time) {
    for_each_rule(this->dispatching_, this->automations_, [&time](RuntimeAutomation &rule) { rule.on_time(time); });
  };
  if (this->last_check_.has_value()) {
    ESPTime &last = *this->last_check_;
    if (last > now && last.timestamp - now.timestamp > MAX_TIMESTAMP_DRIFT) {
      ESP_LOGW(TAG, "Time has jumped back");
    } else if (last >= now) {
      return;
    } else if (now.timestamp - last.timestamp > MAX_TIMESTAMP_DRIFT) {
      ESP_LOGW(TAG, "Time has jumped ahead");
      this->last_check_ = now;
      return;
    }
    while (true) {
      last.increment_second();
      if (last >= now)
        break;
      fire(last);
    }
  }
  this->last_check_ = now;
  fire(now);
}

// --- Mutators: any task in, loop task does the work ---

bool AutomationStorage::run_on_loop_(std::function<bool()> &&job) {
  // The scheduler drops deferred items of a failed component, so a cross-task call would
  // block for LOOP_JOB_TIMEOUT_MS and an inline one would edit state that never reaches flash.
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Automation storage is not available");
    return false;
  }
#ifdef USE_ESP32
  if (this->loop_task_ != nullptr && xTaskGetCurrentTaskHandle() != this->loop_task_) {
    enum class JobState : uint8_t { PENDING, RUNNING, DONE, ABANDONED };
    struct LoopJob {
      std::function<bool()> fn;
      std::atomic<JobState> state{JobState::PENDING};
      bool result{false};
    };
    auto shared = std::make_shared<LoopJob>();
    shared->fn = std::move(job);
    this->defer([shared]() {
      // Only a job still pending may start: an abandoned one has been reported as failed.
      JobState expected = JobState::PENDING;
      if (!shared->state.compare_exchange_strong(expected, JobState::RUNNING))
        return;
      shared->result = shared->fn();
      shared->state = JobState::DONE;
    });
    for (uint32_t waited = 0; shared->state != JobState::DONE && waited < LOOP_JOB_TIMEOUT_MS; waited += 2)
      vTaskDelay(pdMS_TO_TICKS(2));
    JobState expected = JobState::PENDING;
    if (shared->state.compare_exchange_strong(expected, JobState::ABANDONED)) {
      ESP_LOGE(TAG, "Loop task did not run the request in time");
      return false;
    }
    // It started at the deadline: it will finish, and the caller gets the truth.
    while (shared->state != JobState::DONE)
      vTaskDelay(pdMS_TO_TICKS(2));
    return shared->result;
  }
#endif
  if (this->dispatching_ > 0) {
    ESP_LOGE(TAG, "Rules cannot be edited from inside a rule's own action");
    return false;
  }
  return job();
}

uint32_t AutomationStorage::add_automation(const AutomationConfig &config) {
  auto copy = std::make_shared<AutomationConfig>(config);
  auto assigned = std::make_shared<uint32_t>(0);
  if (!this->run_on_loop_([this, copy, assigned]() {
        *assigned = this->add_automation_(*copy);
        return *assigned != 0;
      }))
    return 0;
  return *assigned;
}

bool AutomationStorage::update_automation(uint32_t id, const AutomationConfig &new_config) {
  auto copy = std::make_shared<AutomationConfig>(new_config);
  return this->run_on_loop_([this, id, copy]() { return this->update_automation_(id, *copy); });
}

bool AutomationStorage::remove_automation(uint32_t id) {
  return this->run_on_loop_([this, id]() { return this->remove_automation_(id); });
}

bool AutomationStorage::set_enable_automation(uint32_t id, bool enable, bool *persisted) {
  auto written = std::make_shared<bool>(false);
  const bool ok = this->run_on_loop_(
      [this, id, enable, written]() { return this->set_enable_automation_(id, enable, written.get()); });
  if (persisted != nullptr)
    *persisted = ok && *written;
  return ok;
}

void AutomationStorage::reset_all() {
  this->run_on_loop_([this]() {
    this->reset_all_();
    return true;
  });
}

uint32_t AutomationStorage::add_automation_(const AutomationConfig &config) {
  if (this->config_storage_.size() >= MAX_AUTOMATIONS) {
    ESP_LOGE(TAG, "Refusing to add '%s': already at the %u automation limit", config.name.c_str(),
             static_cast<unsigned>(MAX_AUTOMATIONS));
    return 0;
  }
  // The name decides the file, so a name in use would replace that file. A file nothing loaded
  // is one the loader refused, and it is kept for its author.
  if (this->is_name_taken(config.name)) {
    ESP_LOGE(TAG, "Refusing to add '%s': that name is already in use", config.name.c_str());
    return 0;
  }
  if (file_exists(this->get_filepath_for_name_(config.name))) {
    ESP_LOGE(TAG, "Refusing to add '%s': a file the loader refused has that name", config.name.c_str());
    return 0;
  }
  if (!this->fits_a_file_(config)) {
    ESP_LOGE(TAG, "Refusing to add '%s': it does not fit a %u byte file", config.name.c_str(),
             static_cast<unsigned>(MAX_FILE_BYTES));
    return 0;
  }

  AutomationConfig cfg = config;
  cfg.id = this->allocate_id_();
  cfg.file = this->sanitize_filename_(cfg.name) + ".json";

  auto automation = RuntimeAutomation::build(this, cfg);
  if (automation == nullptr) {
    ESP_LOGE(TAG, "Failed to create automation '%s'", cfg.name.c_str());
    return 0;
  }
  this->subscribe_(*automation);

  this->config_storage_.add_config(cfg);
  this->automations_.push_back(std::move(automation));

  if (!this->save_automation_to_file_(cfg))
    ESP_LOGW(TAG, "Automation '%s' (id=%u) created but not saved", cfg.name.c_str(), static_cast<unsigned>(cfg.id));
  ESP_LOGD(TAG, "Added automation '%s' id=%u", cfg.name.c_str(), static_cast<unsigned>(cfg.id));
  return cfg.id;
}

bool AutomationStorage::update_automation_(uint32_t id, const AutomationConfig &new_config) {
  int found = this->find_automation_index_by_id_(id);
  if (found < 0) {
    ESP_LOGW(TAG, "Automation id=%u not found", static_cast<unsigned>(id));
    return false;
  }
  auto index = static_cast<size_t>(found);

  if (this->is_name_taken(new_config.name, id)) {
    ESP_LOGE(TAG, "Refusing to rename automation id=%u to '%s': that name is already in use", static_cast<unsigned>(id),
             new_config.name.c_str());
    return false;
  }

  AutomationConfig cfg = new_config;
  cfg.id = id;
  const AutomationConfig &current = this->config_storage_.get_all_configs()[index];
  const std::string old_file = current.file;
  // A rule keeps its file unless its name changes: the file may not be the canonical one when
  // that name was taken by a file the loader refused.
  const bool renaming = this->sanitize_filename_(current.name) != this->sanitize_filename_(cfg.name);
  cfg.file = renaming ? this->sanitize_filename_(cfg.name) + ".json" : old_file;
  if (renaming && file_exists(this->get_filepath_for_name_(cfg.name))) {
    ESP_LOGE(TAG, "Refusing to rename automation id=%u to '%s': a file the loader refused has that name",
             static_cast<unsigned>(id), cfg.name.c_str());
    return false;
  }
  if (!this->fits_a_file_(cfg)) {
    ESP_LOGE(TAG, "Refusing to update automation id=%u: it does not fit a %u byte file", static_cast<unsigned>(id),
             static_cast<unsigned>(MAX_FILE_BYTES));
    return false;
  }

  auto automation = RuntimeAutomation::build(this, cfg);
  if (automation == nullptr) {
    ESP_LOGE(TAG, "Failed to create updated automation '%s' id=%u", cfg.name.c_str(), static_cast<unsigned>(id));
    return false;
  }
  this->subscribe_(*automation);

  this->automations_[index] = std::move(automation);
  this->config_storage_.update_config(static_cast<uint8_t>(index), &cfg);

  // Write the new file before dropping the old one.
  if (!this->save_automation_to_file_(cfg)) {
    ESP_LOGW(TAG, "Automation '%s' id=%u updated but not saved", cfg.name.c_str(), static_cast<unsigned>(id));
  } else if (renaming) {
    this->delete_file_(old_file);
  }
  ESP_LOGD(TAG, "Updated automation '%s' id=%u", cfg.name.c_str(), static_cast<unsigned>(id));
  return true;
}

bool AutomationStorage::remove_automation_(uint32_t id) {
  int found = this->find_automation_index_by_id_(id);
  if (found < 0) {
    ESP_LOGW(TAG, "Automation id=%u not found", static_cast<unsigned>(id));
    return false;
  }
  auto index = static_cast<size_t>(found);
  const AutomationConfig &config = this->config_storage_.get_all_configs()[index];
  const std::string name = config.name;
  const std::string file = config.file;

  this->automations_.erase(this->automations_.begin() + found);
  this->config_storage_.remove_config(static_cast<uint8_t>(index));
  this->delete_file_(file);
  ESP_LOGD(TAG, "Removed automation '%s' id=%u", name.c_str(), static_cast<unsigned>(id));
  return true;
}

bool AutomationStorage::set_enable_automation_(uint32_t id, bool enable, bool *persisted) {
  int found = this->find_automation_index_by_id_(id);
  if (found < 0) {
    ESP_LOGW(TAG, "Automation id=%u not found", static_cast<unsigned>(id));
    return false;
  }
  auto index = static_cast<size_t>(found);
  if (this->automations_[index] != nullptr)
    this->automations_[index]->set_enabled(enable);
  AutomationConfig *config = this->config_storage_.get_config(static_cast<uint8_t>(index));
  // The write can fail on its own: the rule is live either way, but the caller has to be able
  // to say whether the change survives a reboot.
  bool written = false;
  if (config != nullptr) {
    config->enabled = enable;
    written = this->save_automation_to_file_(*config);
  }
  if (persisted != nullptr)
    *persisted = written;
  if (!written)
    ESP_LOGW(TAG, "Automation id=%u %s, but the change was not saved", static_cast<unsigned>(id),
             enable ? "enabled" : "disabled");
  return true;
}

void AutomationStorage::reset_all_() {
  this->automations_.clear();
  for (const auto &config : this->config_storage_.get_all_configs())
    this->delete_file_(config.file);
  this->config_storage_.clear();
  this->next_id_ = 1;
  ESP_LOGD(TAG, "All automations removed");
}

// --- Files ---

std::string AutomationStorage::sanitize_filename_(const std::string &name) const {
  std::string result;
  result.reserve(name.length());
  for (char c : name) {
    auto byte = static_cast<unsigned char>(c);
    // Non-ASCII passes through: dropped, every Cyrillic name would map to the same file.
    if (byte >= 0x80) {
      result += c;
    } else if (std::isalnum(byte) != 0) {
      result += static_cast<char>(std::tolower(byte));
    } else if (c == ' ' || c == '-' || c == '_') {
      result += '_';
    }
  }

  std::string cleaned;
  bool prev_underscore = false;
  for (char c : result) {
    if (c == '_') {
      if (!prev_underscore) {
        cleaned += c;
        prev_underscore = true;
      }
    } else {
      cleaned += c;
      prev_underscore = false;
    }
  }

  size_t start = cleaned.find_first_not_of('_');
  size_t end = cleaned.find_last_not_of('_');
  if (start == std::string::npos)
    return "automation";

  std::string trimmed = truncate_utf8(cleaned.substr(start, end - start + 1), MAX_FILENAME_BYTES);
  while (!trimmed.empty() && trimmed.back() == '_')
    trimmed.pop_back();
  return trimmed.empty() ? "automation" : trimmed;
}

std::string AutomationStorage::get_folder_path_() const {
  return this->storage_backend_->get_base_path() + "/" + this->folder_path_;
}

std::string AutomationStorage::get_filepath_for_name_(const std::string &name) const {
  return this->get_folder_path_() + "/" + this->sanitize_filename_(name) + ".json";
}

bool AutomationStorage::is_name_taken(const std::string &name, uint32_t exclude_id) const {
  std::string wanted = this->sanitize_filename_(name);
  for (const auto &config : this->config_storage_.get_all_configs()) {
    if (config.id == exclude_id)
      continue;
    if (this->sanitize_filename_(config.name) == wanted)
      return true;
  }
  return false;
}

uint32_t AutomationStorage::allocate_id_() {
  // Skip ids in use (a hand-written file can carry any), never hand out 0, and stay within
  // what a timer id can carry.
  while (true) {
    if (this->next_id_ == 0 || this->next_id_ > MAX_RULE_ID)
      this->next_id_ = 1;
    if (this->find_automation_index_by_id_(this->next_id_) < 0)
      return this->next_id_++;
    this->next_id_++;
  }
}

bool AutomationStorage::ensure_directory_exists_(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0)
    return S_ISDIR(st.st_mode);
  if (mkdir(path.c_str(), 0755) == 0) {
    ESP_LOGD(TAG, "Created directory: %s", path.c_str());
    return true;
  }
  ESP_LOGE(TAG, "Failed to create directory: %s", path.c_str());
  return false;
}

bool AutomationStorage::load_automation_from_file_(const std::string &filepath) {
  FILE *file = fopen(filepath.c_str(), "r");
  if (file == nullptr) {
    ESP_LOGW(TAG, "Failed to open file: %s", filepath.c_str());
    return false;
  }
  fseek(file, 0, SEEK_END);
  int64_t file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (file_size <= 0 || file_size > static_cast<int64_t>(MAX_FILE_BYTES)) {
    ESP_LOGW(TAG, "Invalid file size: %" PRId64 " bytes for %s", file_size, filepath.c_str());
    fclose(file);
    return false;
  }

  const auto byte_count = static_cast<size_t>(file_size);
  std::unique_ptr<char[]> json_data(new char[byte_count + 1]);
  size_t read_size = fread(json_data.get(), 1, byte_count, file);
  fclose(file);
  if (read_size != byte_count) {
    ESP_LOGE(TAG, "Failed to read file completely: %s", filepath.c_str());
    return false;
  }
  json_data[read_size] = '\0';

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json_data.get());
  if (error) {
    ESP_LOGE(TAG, "JSON parse error in %s: %s", filepath.c_str(), error.c_str());
    return false;
  }

  AutomationConfig config;
  if (!config.deserialize(doc.as<JsonObject>())) {
    ESP_LOGE(TAG, "Failed to deserialize automation from %s", filepath.c_str());
    return false;
  }
  config.file = filepath.substr(filepath.rfind('/') + 1);
  this->config_storage_.add_config(config);
  ESP_LOGD(TAG, "Loaded automation '%s' from %s", config.name.c_str(), filepath.c_str());
  return true;
}

// serialize() rebuilds every object_id from the live entity, so a reference that does not
// resolve would be written back as "". The file is the only record of it: keep it as it is.
static bool entity_missing(const ConditionConfig &condition) {
  switch (condition.type) {
    case ConditionType::INPUT:
      return find_binary_sensor(condition.sensor_id) == nullptr;
    case ConditionType::TEMPERATURE:
      return find_sensor(condition.sensor_id) == nullptr;
    case ConditionType::AND:
    case ConditionType::OR:
    case ConditionType::XOR:
      for (const auto &sub : condition.sub_conditions) {
        if (entity_missing(sub))
          return true;
      }
      return false;
    default:
      return false;
  }
}

static bool entity_missing(const ActionConfig &action) {
  return action.source == SourceAction::SWITCH && find_switch(action.params.switch_action.switch_id) == nullptr;
}

static bool entity_missing(const AutomationConfig &config) {
  for (const auto &trigger : config.triggers) {
    switch (trigger.source) {
      case SourceTrigger::INPUT:
        if (find_binary_sensor(trigger.params.input.input_id) == nullptr)
          return true;
        break;
      case SourceTrigger::SWITCH:
        if (find_switch(trigger.params.switch_trigger.switch_id) == nullptr)
          return true;
        break;
      case SourceTrigger::TEMPERATURE:
        if (find_sensor(trigger.params.temperature.sensor_id) == nullptr)
          return true;
        break;
      default:
        break;
    }
  }
  if (config.condition.is_valid() && entity_missing(config.condition))
    return true;
  for (const auto &action : config.actions) {
    if (entity_missing(action))
      return true;
  }
  for (const auto &action : config.else_actions) {
    if (entity_missing(action))
      return true;
  }
  return false;
}

bool AutomationStorage::fits_a_file_(const AutomationConfig &config) const {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  config.serialize(obj);
  return measureJson(doc) <= MAX_FILE_BYTES;
}

bool AutomationStorage::save_automation_to_file_(const AutomationConfig &config) {
  const std::string filepath =
      config.file.empty() ? this->get_filepath_for_name_(config.name) : this->get_folder_path_() + "/" + config.file;

  if (entity_missing(config)) {
    ESP_LOGW(TAG, "Not writing '%s': an entity it names is missing and the reference would be lost",
             config.name.c_str());
    return false;
  }

  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  config.serialize(obj);
  size_t json_size = measureJson(doc);
  if (json_size == 0 || json_size > MAX_FILE_BYTES) {
    ESP_LOGE(TAG, "Failed to serialize automation '%s': %u bytes", config.name.c_str(),
             static_cast<unsigned>(json_size));
    return false;
  }
  std::unique_ptr<char[]> json_buffer(new char[json_size + 1]);
  serializeJson(doc, json_buffer.get(), json_size + 1);

  // Written beside the target and renamed over it: a write that fails or loses power leaves
  // the old file whole. The close is where a full filesystem shows up.
  const std::string tmp = filepath + ".tmp";
  filesystem_storage_abstract::FilesystemStorageAbstract::Access use(this->storage_backend_);
  if (!use) {
    ESP_LOGW(TAG, "Storage is being formatted: not saving '%s'", config.name.c_str());
    return false;
  }
  FILE *file = fopen(tmp.c_str(), "w");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open '%s' for writing", tmp.c_str());
    return false;
  }
  const size_t written = fwrite(json_buffer.get(), 1, json_size, file);
  const bool closed = fclose(file) == 0;
  if (written != json_size || !closed || rename(tmp.c_str(), filepath.c_str()) != 0) {
    ESP_LOGE(TAG, "Failed to write '%s'", filepath.c_str());
    remove(tmp.c_str());
    return false;
  }
  ESP_LOGD(TAG, "Saved automation '%s' to '%s' (%u bytes)", config.name.c_str(), filepath.c_str(),
           static_cast<unsigned>(json_size));
  return true;
}

bool AutomationStorage::delete_file_(const std::string &filename) {
  const std::string filepath = this->get_folder_path_() + "/" + filename;
  filesystem_storage_abstract::FilesystemStorageAbstract::Access use(this->storage_backend_);
  if (!use)
    return false;
  if (remove(filepath.c_str()) == 0) {
    ESP_LOGD(TAG, "Deleted automation file: %s", filepath.c_str());
    return true;
  }
  ESP_LOGW(TAG, "Failed to delete file: %s", filepath.c_str());
  return false;
}

// The folder is writable by hand, so ids and names are re-established from what was loaded.
// Returns which configs changed, index-aligned with config_storage_.
std::vector<bool> AutomationStorage::resolve_duplicates_() {
  std::vector<bool> changed(this->config_storage_.size(), false);

  uint32_t max_id = 0;
  for (size_t i = 0; i < this->config_storage_.size(); i++) {
    AutomationConfig *cfg = this->config_storage_.get_config(static_cast<uint8_t>(i));
    if (cfg != nullptr && cfg->id > max_id && cfg->id <= MAX_RULE_ID)
      max_id = cfg->id;
  }
  this->next_id_ = max_id + 1;

  std::vector<uint32_t> seen;
  seen.reserve(this->config_storage_.size());
  for (size_t i = 0; i < this->config_storage_.size(); i++) {
    AutomationConfig *cfg = this->config_storage_.get_config(static_cast<uint8_t>(i));
    if (cfg == nullptr)
      continue;
    if (cfg->id == 0 || cfg->id > MAX_RULE_ID || std::find(seen.begin(), seen.end(), cfg->id) != seen.end()) {
      uint32_t fresh = this->allocate_id_();
      ESP_LOGD(TAG, "Automation '%s': id %u -> %u", cfg->name.c_str(), static_cast<unsigned>(cfg->id),
               static_cast<unsigned>(fresh));
      cfg->id = fresh;
      changed[i] = true;
    }
    seen.push_back(cfg->id);
  }

  for (size_t i = 0; i < this->config_storage_.size(); i++) {
    AutomationConfig *cfg = this->config_storage_.get_config(static_cast<uint8_t>(i));
    if (cfg == nullptr || !this->is_name_taken(cfg->name, cfg->id))
      continue;
    std::string original = cfg->name;
    for (uint32_t suffix = 2; suffix <= MAX_AUTOMATIONS + 2; suffix++) {
      std::string tail = " " + std::to_string(suffix);
      std::string candidate = truncate_utf8(original, MAX_FILENAME_BYTES - tail.size()) + tail;
      if (!this->is_name_taken(candidate, cfg->id)) {
        cfg->name = candidate;
        break;
      }
    }
    if (cfg->name == original) {
      ESP_LOGE(TAG, "No free name for a second '%s'; leaving the duplicate as it is", original.c_str());
      continue;
    }
    ESP_LOGW(TAG, "Two automations named '%s'; renamed one to '%s'", original.c_str(), cfg->name.c_str());
    changed[i] = true;
  }
  return changed;
}

// Put each config in the file its name maps to. Two rounds so a config squatting on another's
// canonical filename vacates before the owner writes there.
void AutomationStorage::normalize_filenames_(const std::vector<bool> &changed) {
  const size_t count = this->config_storage_.size();
  if (count == 0)
    return;

  std::vector<std::string> canonical;
  std::vector<std::string> loaded;
  canonical.reserve(count);
  loaded.reserve(count);
  for (const auto &config : this->config_storage_.get_all_configs()) {
    canonical.push_back(this->sanitize_filename_(config.name) + ".json");
    loaded.push_back(config.file);
  }

  const std::string folder_path = this->get_folder_path_();
  std::vector<std::string> occupied;

  for (int round = 0; round < 2; round++) {
    for (size_t i = 0; i < count; i++) {
      AutomationConfig *cfg = this->config_storage_.get_config(static_cast<uint8_t>(i));
      if (cfg == nullptr)
        continue;

      const std::string from = cfg->file;
      const bool moved = from != canonical[i];
      const bool restamped = i < changed.size() && changed[i];
      if (!moved && !restamped)
        continue;

      const bool vacating = moved && std::find(canonical.begin(), canonical.end(), from) != canonical.end();
      if (vacating != (round == 0))
        continue;

      if (std::find(occupied.begin(), occupied.end(), canonical[i]) != occupied.end()) {
        ESP_LOGW(TAG, "Not writing '%s': another automation is still stored there", canonical[i].c_str());
        continue;
      }
      // A target nothing loaded from is a file the loader refused: it stays, the rule stays put.
      if (moved && std::find(loaded.begin(), loaded.end(), canonical[i]) == loaded.end() &&
          file_exists(folder_path + "/" + canonical[i])) {
        ESP_LOGW(TAG, "Not writing '%s': a file the loader refused is there; leaving '%s' in place",
                 canonical[i].c_str(), from.c_str());
        continue;
      }

      App.feed_wdt();
      cfg->file = canonical[i];
      if (!this->save_automation_to_file_(*cfg)) {
        cfg->file = from;
        ESP_LOGW(TAG, "Could not write '%s'; leaving '%s' in place", canonical[i].c_str(), from.c_str());
        if (moved)
          occupied.push_back(from);
        continue;
      }

      if (!moved)
        continue;
      ESP_LOGD(TAG, "Renamed automation file for '%s': %s -> %s", cfg->name.c_str(), from.c_str(),
               canonical[i].c_str());
      if (vacating)
        continue;
      const std::string filepath = folder_path + "/" + from;
      if (remove(filepath.c_str()) != 0) {
        // Blank it: loaded again it would be renamed to a fresh copy every boot.
        ESP_LOGW(TAG, "Failed to delete stale automation file: %s", filepath.c_str());
        FILE *blank = fopen(filepath.c_str(), "w");
        if (blank != nullptr)
          fclose(blank);
      }
    }
  }
}

int AutomationStorage::find_automation_index_by_id_(uint32_t id) {
  const auto &configs = this->config_storage_.get_all_configs();
  for (size_t i = 0; i < configs.size(); i++) {
    if (configs[i].id == id)
      return static_cast<int>(i);
  }
  return -1;
}

// --- dump_config ---

void AutomationStorage::dump_config() {
  ESP_LOGCONFIG(TAG, "Automations:");
  if (this->storage_backend_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Storage: %s", this->storage_backend_->get_filesystem_type());
    ESP_LOGCONFIG(TAG, "  Folder: %s", this->get_folder_path_().c_str());
  }
  ESP_LOGCONFIG(TAG, "  Time source: %s", YESNO(this->rtc_ != nullptr));
  ESP_LOGCONFIG(TAG, "  Total: %u", static_cast<unsigned>(this->config_storage_.size()));

  const auto &configs = this->config_storage_.get_all_configs();
  for (size_t i = 0; i < configs.size(); i++) {
    const auto &config = configs[i];
    ESP_LOGCONFIG(TAG, "  [%u] id=%u '%s' %s %s", static_cast<unsigned>(i), static_cast<unsigned>(config.id),
                  config.name.c_str(), config.enabled ? "enabled" : "disabled",
                  (i < this->automations_.size() && this->automations_[i] != nullptr) ? "" : "(not built)");
    for (const auto &trigger : config.triggers)
      this->print_trigger_info_(trigger, 4);
    if (config.condition.is_valid())
      this->print_condition_info_(config.condition, 4);
    for (const auto &action : config.actions)
      this->print_action_info_(action, 4);
    for (const auto &action : config.else_actions)
      this->print_action_info_(action, 6);
  }
}

void AutomationStorage::print_trigger_info_(const TriggerConfig &trigger, int indent) {
  std::string pad(indent, ' ');
  switch (trigger.source) {
    case SourceTrigger::INPUT:
      ESP_LOGCONFIG(TAG, "%sTrigger: input %s 0x%08X", pad.c_str(),
                    EnumUtils::input_trigger_type_to_string(trigger.params.input.type),
                    static_cast<unsigned>(trigger.params.input.input_id));
      break;
    case SourceTrigger::SWITCH:
      ESP_LOGCONFIG(TAG, "%sTrigger: switch %s 0x%08X", pad.c_str(),
                    EnumUtils::switch_trigger_type_to_string(trigger.params.switch_trigger.type),
                    static_cast<unsigned>(trigger.params.switch_trigger.switch_id));
      break;
    case SourceTrigger::TEMPERATURE:
      ESP_LOGCONFIG(TAG, "%sTrigger: temperature %s 0x%08X (%.2f / %.2f..%.2f)", pad.c_str(),
                    EnumUtils::temperature_trigger_type_to_string(trigger.params.temperature.type),
                    static_cast<unsigned>(trigger.params.temperature.sensor_id), trigger.params.temperature.threshold,
                    trigger.params.temperature.min_threshold, trigger.params.temperature.max_threshold);
      break;
    case SourceTrigger::CRON:
      ESP_LOGCONFIG(
          TAG, "%sTrigger: cron '%s' (%s)", pad.c_str(), trigger.cron_string().c_str(),
          trigger.cron_preset.has_value() ? EnumUtils::cron_preset_to_string(*trigger.cron_preset) : "no preset");
      break;
    case SourceTrigger::STARTUP:
      ESP_LOGCONFIG(TAG, "%sTrigger: startup", pad.c_str());
      break;
    default:
      ESP_LOGCONFIG(TAG, "%sTrigger: none", pad.c_str());
      break;
  }
}

void AutomationStorage::print_condition_info_(const ConditionConfig &condition, int indent) {
  std::string pad(indent, ' ');
  switch (condition.type) {
    case ConditionType::INPUT:
      ESP_LOGCONFIG(TAG, "%sCondition: input 0x%08X is %s", pad.c_str(), static_cast<unsigned>(condition.sensor_id),
                    EnumUtils::input_condition_state_to_string(condition.state));
      break;
    case ConditionType::TEMPERATURE:
      ESP_LOGCONFIG(TAG, "%sCondition: temperature %s 0x%08X (%.2f / %.2f..%.2f)", pad.c_str(),
                    EnumUtils::temperature_condition_type_to_string(condition.temperature_type),
                    static_cast<unsigned>(condition.sensor_id), condition.threshold, condition.min_threshold,
                    condition.max_threshold);
      break;
    case ConditionType::AND:
    case ConditionType::OR:
    case ConditionType::XOR:
      ESP_LOGCONFIG(TAG, "%sCondition: %s", pad.c_str(), EnumUtils::condition_type_to_string(condition.type));
      for (const auto &sub : condition.sub_conditions)
        this->print_condition_info_(sub, indent + 2);
      break;
    default:
      break;
  }
}

void AutomationStorage::print_action_info_(const ActionConfig &action, int indent) {
  std::string pad(indent, ' ');
  switch (action.source) {
    case SourceAction::SWITCH:
      ESP_LOGCONFIG(TAG, "%sAction: switch %s 0x%08X", pad.c_str(),
                    EnumUtils::switch_action_type_to_string(action.params.switch_action.type),
                    static_cast<unsigned>(action.params.switch_action.switch_id));
      break;
    case SourceAction::DELAY:
      ESP_LOGCONFIG(TAG, "%sAction: delay %u ms", pad.c_str(), static_cast<unsigned>(action.params.delay.delay_ms));
      break;
    default:
      ESP_LOGCONFIG(TAG, "%sAction: none", pad.c_str());
      break;
  }
}

}  // namespace automations
}  // namespace esphome
