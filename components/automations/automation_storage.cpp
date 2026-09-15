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

AutomationStorage::AutomationStorage() { global_automation_storage = this; }

// --- Setup and runtime ---

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

  // The file each loaded config came from, index-aligned with config_storage_.
  std::vector<std::string> loaded_files;
  DIR *dir = opendir(folder_path.c_str());
  if (dir != nullptr) {
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
      if (this->load_automation_from_file_(folder_path + "/" + filename))
        loaded_files.push_back(filename);
    }
    closedir(dir);
  }

  this->normalize_filenames_(loaded_files, this->resolve_duplicates_());
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
    for (const auto &automation : this->automations_) {
      if (automation != nullptr)
        automation->on_startup();
    }
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
                              sub->entity->add_on_state_callback(
                                  [sub](bool state) { sub->engine->dispatch_binary_sensor_(sub->entity, state); });
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

void AutomationStorage::dispatch_binary_sensor_(binary_sensor::BinarySensor *entity, bool state) {
  for (size_t i = 0; i < this->automations_.size(); i++) {
    if (this->automations_[i] != nullptr)
      this->automations_[i]->on_binary_sensor(entity, state);
  }
}

void AutomationStorage::dispatch_switch_(switch_::Switch *entity, bool state) {
  for (size_t i = 0; i < this->automations_.size(); i++) {
    if (this->automations_[i] != nullptr)
      this->automations_[i]->on_switch(entity, state);
  }
}

void AutomationStorage::dispatch_sensor_(sensor::Sensor *entity, float value) {
  for (size_t i = 0; i < this->automations_.size(); i++) {
    if (this->automations_[i] != nullptr)
      this->automations_[i]->on_sensor(entity, value);
  }
}

// Same catch-up and clock-jump handling as the core cron trigger, for all rules at once.
void AutomationStorage::check_time_() {
  ESPTime now = this->rtc_->now();
  if (!now.is_valid())
    return;
  auto fire = [this](const ESPTime &time) {
    for (size_t i = 0; i < this->automations_.size(); i++) {
      if (this->automations_[i] != nullptr)
        this->automations_[i]->on_time(time);
    }
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
#ifdef USE_ESP32
  if (this->loop_task_ != nullptr && xTaskGetCurrentTaskHandle() != this->loop_task_) {
    struct LoopJob {
      std::function<bool()> fn;
      std::atomic<bool> done{false};
      bool result{false};
    };
    auto shared = std::make_shared<LoopJob>();
    shared->fn = std::move(job);
    this->defer([shared]() {
      shared->result = shared->fn();
      shared->done = true;
    });
    for (uint32_t waited = 0; !shared->done && waited < LOOP_JOB_TIMEOUT_MS; waited += 2)
      vTaskDelay(pdMS_TO_TICKS(2));
    if (!shared->done) {
      ESP_LOGE(TAG, "Loop task did not run the request in time");
      return false;
    }
    return shared->result;
  }
#endif
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

bool AutomationStorage::set_enable_automation(uint32_t id, bool enable) {
  return this->run_on_loop_([this, id, enable]() { return this->set_enable_automation_(id, enable); });
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
  // The name decides the file, so a name in use would replace that file.
  if (this->is_name_taken(config.name)) {
    ESP_LOGE(TAG, "Refusing to add '%s': that name is already in use", config.name.c_str());
    return 0;
  }

  AutomationConfig cfg = config;
  cfg.id = this->allocate_id_();

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
  const std::string old_name = this->config_storage_.get_all_configs()[index].name;

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
  } else if (this->sanitize_filename_(old_name) != this->sanitize_filename_(cfg.name)) {
    // The old name can belong to another automation whose file was hand-renamed.
    if (this->is_name_taken(old_name, id)) {
      ESP_LOGW(TAG, "Not deleting '%s': it belongs to another automation", old_name.c_str());
    } else {
      this->delete_automation_file_(old_name);
    }
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
  const std::string name = this->config_storage_.get_all_configs()[index].name;

  this->automations_.erase(this->automations_.begin() + found);
  this->config_storage_.remove_config(static_cast<uint8_t>(index));
  this->delete_automation_file_(name);
  ESP_LOGD(TAG, "Removed automation '%s' id=%u", name.c_str(), static_cast<unsigned>(id));
  return true;
}

bool AutomationStorage::set_enable_automation_(uint32_t id, bool enable) {
  int found = this->find_automation_index_by_id_(id);
  if (found < 0) {
    ESP_LOGW(TAG, "Automation id=%u not found", static_cast<unsigned>(id));
    return false;
  }
  auto index = static_cast<size_t>(found);
  if (this->automations_[index] != nullptr)
    this->automations_[index]->set_enabled(enable);
  AutomationConfig *config = this->config_storage_.get_config(static_cast<uint8_t>(index));
  if (config != nullptr) {
    config->enabled = enable;
    this->save_automation_to_file_(*config);
  }
  return true;
}

void AutomationStorage::reset_all_() {
  this->automations_.clear();
  for (const auto &config : this->config_storage_.get_all_configs())
    this->delete_automation_file_(config.name);
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
  // Skip ids in use (a hand-written file can carry any) and never hand out 0.
  while (this->next_id_ == 0 || this->find_automation_index_by_id_(this->next_id_) >= 0)
    this->next_id_++;
  return this->next_id_++;
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
  if (file_size <= 0 || file_size > 16384) {
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
  this->config_storage_.add_config(config);
  ESP_LOGD(TAG, "Loaded automation '%s' from %s", config.name.c_str(), filepath.c_str());
  return true;
}

bool AutomationStorage::save_automation_to_file_(const AutomationConfig &config) {
  std::string filepath = this->get_filepath_for_name_(config.name);

  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  config.serialize(obj);
  size_t json_size = measureJson(doc);
  if (json_size == 0) {
    ESP_LOGE(TAG, "Failed to serialize automation: %s", config.name.c_str());
    return false;
  }
  std::unique_ptr<char[]> json_buffer(new char[json_size + 1]);
  serializeJson(doc, json_buffer.get(), json_size + 1);

  FILE *file = fopen(filepath.c_str(), "w");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open '%s' for writing", filepath.c_str());
    return false;
  }
  size_t written = fwrite(json_buffer.get(), 1, json_size, file);
  fclose(file);
  if (written != json_size) {
    ESP_LOGE(TAG, "Failed to write complete data to '%s'", filepath.c_str());
    return false;
  }
  ESP_LOGD(TAG, "Saved automation '%s' to '%s' (%u bytes)", config.name.c_str(), filepath.c_str(),
           static_cast<unsigned>(json_size));
  return true;
}

bool AutomationStorage::delete_automation_file_(const std::string &name) {
  std::string filepath = this->get_filepath_for_name_(name);
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
    if (cfg != nullptr && cfg->id > max_id)
      max_id = cfg->id;
  }
  this->next_id_ = max_id + 1;

  std::vector<uint32_t> seen;
  seen.reserve(this->config_storage_.size());
  for (size_t i = 0; i < this->config_storage_.size(); i++) {
    AutomationConfig *cfg = this->config_storage_.get_config(static_cast<uint8_t>(i));
    if (cfg == nullptr)
      continue;
    if (cfg->id == 0 || std::find(seen.begin(), seen.end(), cfg->id) != seen.end()) {
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
void AutomationStorage::normalize_filenames_(const std::vector<std::string> &filenames,
                                             const std::vector<bool> &changed) {
  size_t count = std::min(filenames.size(), static_cast<size_t>(this->config_storage_.size()));
  if (count == 0)
    return;

  std::vector<std::string> canonical;
  canonical.reserve(this->config_storage_.size());
  for (const auto &config : this->config_storage_.get_all_configs())
    canonical.push_back(this->sanitize_filename_(config.name) + ".json");

  std::string folder_path = this->get_folder_path_();
  std::vector<std::string> occupied;

  for (int round = 0; round < 2; round++) {
    for (size_t i = 0; i < count; i++) {
      AutomationConfig *cfg = this->config_storage_.get_config(static_cast<uint8_t>(i));
      if (cfg == nullptr)
        continue;

      bool moved = filenames[i] != canonical[i];
      bool restamped = i < changed.size() && changed[i];
      if (!moved && !restamped)
        continue;

      bool vacating = moved && std::find(canonical.begin(), canonical.end(), filenames[i]) != canonical.end();
      if (vacating != (round == 0))
        continue;

      if (std::find(occupied.begin(), occupied.end(), canonical[i]) != occupied.end()) {
        ESP_LOGW(TAG, "Not writing '%s': another automation is still stored there", canonical[i].c_str());
        continue;
      }

      App.feed_wdt();
      if (!this->save_automation_to_file_(*cfg)) {
        ESP_LOGW(TAG, "Could not write '%s'; leaving '%s' in place", canonical[i].c_str(), filenames[i].c_str());
        if (moved)
          occupied.push_back(filenames[i]);
        continue;
      }

      if (!moved)
        continue;
      ESP_LOGD(TAG, "Renamed automation file for '%s': %s -> %s", cfg->name.c_str(), filenames[i].c_str(),
               canonical[i].c_str());
      if (vacating)
        continue;
      std::string filepath = folder_path + "/" + filenames[i];
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
      ESP_LOGCONFIG(TAG, "%sTrigger: cron (%s)", pad.c_str(), EnumUtils::cron_preset_to_string(trigger.cron_preset));
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
