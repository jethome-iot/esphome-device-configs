#include "schedule_engine.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>
#include "esphome/components/switch_hold/switch_hold.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "week_clock.h"
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_CLIMATE_HUB
#include "esphome/components/climate_hub/climate_hub.h"
#endif

namespace esphome::schedules {

static const char *const TAG = "schedules";

// A local clock that moves this much more, or less, than the loop's own count is a jump: a
// clock set by hand or by SNTP, a new time zone, a DST switch. Anything smaller is drift.
static const int64_t JUMP_TOLERANCE_S = 60;
// The id search at create; files left in the folder count, so it can run out.
static const unsigned MAX_ID_SUFFIX = 999;

static const char *const NOT_FOUND = "Schedule not found";
// Its fields this firmware does not know would be lost.
static const char *const NEWER_FILE = "A newer firmware wrote this schedule; update the firmware to change it";
// The dashboard's editor opens a blank form at /schedules/new.
static const char *const RESERVED_ID = "new";

const char *state_to_string(ScheduleState state) {
  switch (state) {
    case ScheduleState::DISABLED:
      return "disabled";
    case ScheduleState::WAITING_FOR_CLOCK:
      return "waiting_for_clock";
    case ScheduleState::MISSING_TARGET:
      return "missing_target";
    case ScheduleState::MISSING_PRESET:
      return "missing_preset";
    case ScheduleState::CONFLICT:
      return "conflict";
    case ScheduleState::HELD:
      return "held";
    case ScheduleState::RUNNING:
      return "running";
  }
  return "running";
}

static bool file_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static Result failure(uint16_t code, std::string error) {
  Result result;
  result.ok = false;
  result.code = code;
  result.error = std::move(error);
  return result;
}

static Result success() {
  Result result;
  result.ok = true;
  return result;
}

// A create or update whose file was not written: nothing changed.
static Result not_saved(bool too_large) {
  if (too_large)
    return failure(413, "The schedule's file would be over 16 KiB");
  Result result = failure(500, "The schedule's file could not be written");
  result.persisted = false;
  return result;
}

static_assert(MAX_FILE_BYTES == 16384, "the sentence above names the limit");

static switch_::Switch *find_switch(const std::string &object_id) {
#ifdef USE_SWITCH
  // The hash narrows, the string decides; an internal switch is no one's to schedule.
  const uint32_t key = fnv1_hash(object_id);
  for (auto *sw : App.get_switches()) {
    char buf[OBJECT_ID_MAX_LEN];
    if (sw->get_object_id_hash() == key && !sw->is_internal() && object_id == sw->get_object_id_to(buf).c_str())
      return sw;
  }
#else
  (void) object_id;
#endif
  return nullptr;
}

// A reason as the tail of a warning: "not running: relay ...".
static std::string lower_first(std::string text) {
  if (!text.empty() && text[0] >= 'A' && text[0] <= 'Z')
    text[0] = static_cast<char>(text[0] - 'A' + 'a');
  return text;
}

// For the log: "on", "preset 'eco'", "target 21.5".
static std::string describe(const ScheduleConfig &config, const Point &point) {
  switch (point.type) {
    case ValueType::PRESET:
      return "preset '" + config.preset_of(point) + "'";
    case ValueType::TARGET: {
      char text[64];
      snprintf(text, sizeof(text), "target %.1f", point.target);
      return text;
    }
    default:
      return value_type_to_string(point.type);
  }
}

// --- Setup and the clock ---

void ScheduleEngine::setup() {
  this->dispatcher_.capture_loop_task();
  if (this->storage_ == nullptr || !this->storage_->is_mounted()) {
    ESP_LOGE(TAG, "Storage not mounted");
    this->mark_failed();
    return;
  }
  if (!this->load_()) {
    this->mark_failed();
    return;
  }
#ifdef USE_CLIMATE_HUB
  // The hub sets up first: it tells of a thermostat created, removed, or with other presets.
  if (global_climate_hub != nullptr)
    global_climate_hub->add_on_change_callback([this](const std::string &id) { this->on_climate_change_(id); });
#endif
  switch_hold::add_on_release_callback([this](switch_::Switch *sw) { this->on_released_(sw); });
  // Nothing applies before the first tick that reads a valid clock.
  this->set_interval("tick", 1000, [this]() { this->tick_(); });
}

ESPTime ScheduleEngine::clock_now_() { return this->rtc_ != nullptr ? this->rtc_->now() : ESPTime{}; }

uint64_t ScheduleEngine::mono_ms_() { return millis_64(); }

void ScheduleEngine::tick_() {
  const ESPTime now = this->clock_now_();
  if (!now.is_valid())
    return;
  const int64_t local = local_seconds(now);
  const int64_t minute = local / 60;
  const uint64_t mono = this->mono_ms_();
  if (!this->clock_valid_) {
    this->clock_valid_ = true;
    this->mark_ = ClockMark{local, mono, minute};
    this->start_all_("clock valid");
    return;
  }
  // A stalled loop moves both clocks together; only a clock set apart from it is a jump.
  const int64_t drift = (local - this->mark_.local_s) - static_cast<int64_t>((mono - this->mark_.mono_ms) / 1000);
  this->mark_.local_s = local;
  this->mark_.mono_ms = mono;
  if (std::llabs(drift) > JUMP_TOLERANCE_S) {
    ESP_LOGW(TAG, "Clock moved by %+" PRId64 " s: applying the values in force", drift);
    this->mark_.minute = minute;
    this->start_all_("clock jump");
    return;
  }
  // The same minute, or a small step back: wait until it is passed again.
  if (minute <= this->mark_.minute)
    return;
  const int64_t from = this->mark_.minute;
  this->mark_.minute = minute;
  const uint16_t week = week_minute_of(minute);
  // Each entry once, at the latest point the step passed: a stall of minutes never replays the ones before it.
  std::vector<std::pair<std::string, Point>> due;
  for (const auto &entry : this->entries_) {
    const Point *point = entry->active ? entry->config.in_force(week) : nullptr;
    if (point == nullptr)
      continue;
    const int64_t back = (week - point->minute_of_week + MINUTES_PER_WEEK) % MINUTES_PER_WEEK;
    if (minute - back > from)
      due.emplace_back(entry->config.id, *point);
  }
  for (const auto &item : due)
    this->apply_(item.first, item.second, "switch point");
}

void ScheduleEngine::start_all_(const char *why) {
  std::vector<std::string> ids;
  for (const auto &entry : this->entries_)
    ids.push_back(entry->config.id);
  for (const std::string &id : ids)
    this->start_(id, why);
}

void ScheduleEngine::start_(const std::string &id, const char *why) {
  const Entry *entry = this->find_(id);
  if (entry == nullptr || !entry->active || !this->clock_valid_)
    return;
  const Point *point = entry->config.in_force(week_minute_of(this->mark_.minute));
  if (point != nullptr)
    this->apply_(id, *point, why);
}

void ScheduleEngine::apply_(const std::string &id, const Point &point, const char *why) {
  const Entry *entry = this->find_(id);
  if (entry == nullptr || !entry->active)
    return;
  if (entry->config.target_type == TargetType::RELAY) {
    this->apply_relay_(*entry, point, why);
  } else {
    this->apply_thermostat_(*entry, point, why);
  }
}

void ScheduleEngine::apply_relay_(const Entry &entry, const Point &point, const char *why) {
#ifdef USE_SWITCH
  // Never nullptr: an entry drives its relay only once it found it.
  switch_::Switch *sw = entry.relay;
  // A running thermostat's relay is the thermostat's: the value waits until it lets go.
  const std::string holder = switch_hold::holder(sw);
  if (!holder.empty()) {
    ESP_LOGI(TAG, "Schedule '%s' left '%s' alone: thermostat '%s' drives it", entry.config.name.c_str(),
             sw->get_name().c_str(), holder.c_str());
    return;
  }
  ESP_LOGI(TAG, "Schedule '%s' (%s): '%s' %s", entry.config.name.c_str(), why, sw->get_name().c_str(),
           describe(entry.config, point).c_str());
  // Last: the switch's callbacks may change anything, `entry` included.
  if (point.type == ValueType::ON) {
    sw->turn_on();
  } else {
    sw->turn_off();
  }
#else
  (void) entry;
  (void) point;
  (void) why;
#endif
}

// The calls an automation rule makes, so the hub treats it the same: a stopped thermostat's
// document takes the values and stays stopped, a relay it holds and a calibration as for a rule.
void ScheduleEngine::apply_thermostat_(const Entry &entry, const Point &point, const char *why) {
#ifdef USE_CLIMATE_HUB
  climate_hub::ClimateHub *hub = global_climate_hub;
  const climate_hub::ClimateConfig *thermostat = hub != nullptr ? hub->store().get(entry.config.target_id) : nullptr;
  // Copies: the hub's calls below may change the entry through its change callback.
  const std::string id = entry.config.id;
  const std::string target = entry.config.target_id;
  ESP_LOGI(TAG, "Schedule '%s' (%s): thermostat '%s' %s", entry.config.name.c_str(), why, target.c_str(),
           describe(entry.config, point).c_str());
  climate_hub::Result result;
  if (thermostat == nullptr) {
    result.error = "Thermostat not found";
  } else if (point.type == ValueType::OFF) {
    result = hub->turn_off(target);
  } else if (point.type == ValueType::PRESET) {
    const std::string key = entry.config.preset_of(point);
    const climate_hub::PresetConfig *preset = thermostat->find_preset(key);
    // A preset that keeps the mode would leave an off thermostat off.
    const bool keeps_mode =
        preset != nullptr && !(preset->mode.has_value() && thermostat->supports_mode(*preset->mode));
    result.ok = true;
    if (keeps_mode && thermostat->mode == climate_hub::HubMode::OFF)
      result = hub->turn_on(target);
    if (result.ok)
      result = hub->apply_preset(target, key);
  } else {
    result.ok = true;
    if (thermostat->mode == climate_hub::HubMode::OFF)
      result = hub->turn_on(target);
    if (result.ok)
      result = hub->set_setpoint(target, point.target);
  }
  if (result.ok)
    return;
  // The hub dropped the thermostat or the preset without saying so: what is missing now shows.
  ESP_LOGW(TAG, "Schedule '%s': thermostat '%s': %s", id.c_str(), target.c_str(), result.error.c_str());
  Entry *gone = this->find_(id);
  if (gone != nullptr) {
    this->resolve_(*gone);
    this->refresh_();
  }
#else
  // Never active without the hub: the thermostat is missing.
  (void) entry;
  (void) point;
  (void) why;
#endif
}

void ScheduleEngine::on_climate_change_(const std::string &climate_id) {
  for (auto &entry : this->entries_) {
    if (entry->config.target_type == TargetType::THERMOSTAT && entry->config.target_id == climate_id)
      this->resolve_(*entry);
  }
  // From inside the hub's own change: the value goes in once the loop calls the hub again.
  for (const std::string &id : this->refresh_())
    this->later_([this, id]() { this->start_(id, "thermostat back"); });
}

void ScheduleEngine::on_released_(switch_::Switch *sw) {
  // As an input binding's Follow does: the relay takes the value in force once it is free.
  for (const auto &entry : this->entries_) {
    if (!entry->active || entry->relay != sw)
      continue;
    const std::string id = entry->config.id;
    // Inside the holder's own change: the relay moves once that is done.
    this->later_([this, id]() { this->start_(id, "relay free"); });
  }
}

// --- Loading ---

bool ScheduleEngine::load_() {
  const std::string folder = this->folder_();
  if (!this->ensure_folder_()) {
    ESP_LOGE(TAG, "Cannot create '%s'", folder.c_str());
    return false;
  }
  DIR *dir = opendir(folder.c_str());
  if (dir == nullptr) {
    ESP_LOGE(TAG, "Cannot read '%s'", folder.c_str());
    return false;
  }
  std::vector<std::string> names;
  while (struct dirent *entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
      names.push_back(std::move(name));
  }
  closedir(dir);
  // readdir order is the filesystem's; which files fit under the cap should not be.
  std::sort(names.begin(), names.end());
  for (const std::string &name : names) {
    if (this->entries_.size() >= this->max_schedules_) {
      ESP_LOGW(TAG, "'%s' left alone: at most %u schedules", name.c_str(), static_cast<unsigned>(this->max_schedules_));
      continue;
    }
    // setup() runs before the loop feeds the watchdog.
    App.feed_wdt();
    this->load_file_(folder, name);
  }
  for (auto &entry : this->entries_)
    this->resolve_(*entry);
  this->refresh_();
  ESP_LOGD(TAG, "Loaded %u schedules from '%s'", static_cast<unsigned>(this->entries_.size()), folder.c_str());
  return true;
}

// A file that is refused stays exactly as it is: the folder is writable by hand and over the
// file API, and the file is the only record of what its author meant.
bool ScheduleEngine::load_file_(const std::string &folder, const std::string &filename) {
  const std::string path = folder + "/" + filename;
  const std::string stem = filename.substr(0, filename.size() - 5);
  FILE *file = fopen(path.c_str(), "r");
  if (file == nullptr) {
    ESP_LOGW(TAG, "Cannot open '%s'", path.c_str());
    return false;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size <= 0 || static_cast<size_t>(size) > MAX_FILE_BYTES) {
    ESP_LOGW(TAG, "'%s' refused: %ld bytes", path.c_str(), size);
    fclose(file);
    return false;
  }
  std::string data(static_cast<size_t>(size), '\0');
  const size_t got = fread(&data[0], 1, data.size(), file);
  fclose(file);
  if (got != data.size()) {
    ESP_LOGW(TAG, "'%s' refused: short read", path.c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, data)) {
    ESP_LOGE(TAG, "'%s' refused: not valid JSON", path.c_str());
    return false;
  }
  auto entry = std::make_unique<Entry>();
  ScheduleConfig &config = entry->config;
  std::string error;
  if (!config.deserialize(doc.as<JsonObjectConst>(), true, &error)) {
    ESP_LOGE(TAG, "'%s' refused: %s", path.c_str(), error.c_str());
    return false;
  }
  if (config.id != stem) {
    ESP_LOGE(TAG, "'%s' refused: its id '%s' is not its file name", path.c_str(), config.id.c_str());
    return false;
  }
  if (config.id == RESERVED_ID) {
    ESP_LOGE(TAG, "'%s' refused: the editor cannot open the id '%s'", path.c_str(), RESERVED_ID);
    return false;
  }
  if (config.from_newer_firmware()) {
    ESP_LOGW(TAG, "'%s' is version %u, from a newer firmware: it runs, but changes stay in memory", path.c_str(),
             static_cast<unsigned>(config.version));
  }
  this->resolve_name_(&config);
  // Loaded in file name order, which is id order.
  this->entries_.push_back(std::move(entry));
  return true;
}

// Two documents under one name: the later one by id becomes "<name> 2" and is written back, so
// the next boot agrees.
void ScheduleEngine::resolve_name_(ScheduleConfig *config) {
  if (!this->is_name_taken(config->name, config->id, nullptr))
    return;
  const std::string original = config->name;
  // One of max_schedules + 1 candidates is free whatever the others are called.
  std::string candidate;
  for (unsigned n = 2; candidate.empty() || this->is_name_taken(candidate, config->id, nullptr); n++) {
    const std::string tail = " " + std::to_string(n);
    candidate = trim_name(cut_chars(original, NAME_MAX_LENGTH - tail.size())) + tail;
  }
  config->name = candidate;
  ESP_LOGW(TAG, "'%s': the name '%s' is taken, renamed to '%s'", config->id.c_str(), original.c_str(),
           candidate.c_str());
  if (!this->save_(*config))
    ESP_LOGW(TAG, "'%s': the new name was not written", config->id.c_str());
}

void ScheduleEngine::dump_config() {
  ESP_LOGCONFIG(TAG, "Schedules:");
  if (this->storage_ != nullptr)
    ESP_LOGCONFIG(TAG, "  Folder: %s", this->folder_().c_str());
  ESP_LOGCONFIG(TAG, "  Schedules: %u of %u", static_cast<unsigned>(this->entries_.size()),
                static_cast<unsigned>(this->max_schedules_));
  for (const auto &entry : this->entries_) {
    const ScheduleStatus status = this->status(entry->config.id);
    ESP_LOGCONFIG(TAG, "  '%s' (%s): %s '%s', %s%s%s%s", entry->config.name.c_str(), entry->config.id.c_str(),
                  target_type_to_string(entry->config.target_type), entry->config.target_id.c_str(),
                  state_to_string(status.state), status.reason.empty() ? "" : ": ", status.reason.c_str(),
                  entry->config.from_newer_firmware() ? ", file from a newer firmware" : "");
  }
}

// --- Reads ---

bool ScheduleEngine::run_on_loop(std::function<bool()> &&job) {
  // A failed component is not scheduled, so a deferred job would only wait out the timeout.
  // Its mutators refuse for that same reason, so nothing moves under a job that runs in place.
  if (this->is_failed())
    return job();
  return this->dispatcher_.run_on_loop(this, std::move(job));
}

ScheduleEngine::Entry *ScheduleEngine::find_(const std::string &id) const {
  for (const auto &entry : this->entries_) {
    if (entry->config.id == id)
      return entry.get();
  }
  return nullptr;
}

std::vector<const ScheduleConfig *> ScheduleEngine::schedules() const {
  std::vector<const ScheduleConfig *> out;
  for (const auto &entry : this->entries_)
    out.push_back(&entry->config);
  return out;
}

const ScheduleConfig *ScheduleEngine::get(const std::string &id) const {
  const Entry *entry = this->find_(id);
  return entry == nullptr ? nullptr : &entry->config;
}

std::string ScheduleEngine::document(const std::string &id) const {
  const Entry *entry = this->find_(id);
  if (entry == nullptr)
    return "";
  JsonDocument doc;
  entry->config.serialize(doc.to<JsonObject>());
  std::string json;
  serializeJson(doc, json);
  return json;
}

ScheduleState ScheduleEngine::check_target_(const ScheduleConfig &config, std::string *reason,
                                            switch_::Switch **relay) const {
  *relay = nullptr;
  if (config.target_type == TargetType::RELAY) {
    *relay = find_switch(config.target_id);
    if (*relay != nullptr)
      return ScheduleState::RUNNING;
    *reason = "Relay \"" + config.target_id + "\" is not on this device";
    return ScheduleState::MISSING_TARGET;
  }
#ifdef USE_CLIMATE_HUB
  if (global_climate_hub != nullptr) {
    const climate_hub::ClimateConfig *thermostat = global_climate_hub->store().get(config.target_id);
    if (thermostat == nullptr) {
      *reason = "Thermostat \"" + config.target_id + "\" is not on this device";
      return ScheduleState::MISSING_TARGET;
    }
    for (const std::string &key : config.preset_keys) {
      if (thermostat->find_preset(key) == nullptr) {
        *reason = "Thermostat \"" + thermostat->name + "\" has no preset \"" + key + "\"";
        return ScheduleState::MISSING_PRESET;
      }
    }
    return ScheduleState::RUNNING;
  }
#endif
  *reason = "This device has no thermostats";
  return ScheduleState::MISSING_TARGET;
}

void ScheduleEngine::resolve_(Entry &entry) {
  entry.missing_reason.clear();
  entry.missing = this->check_target_(entry.config, &entry.missing_reason, &entry.relay);
}

const ScheduleEngine::Entry *ScheduleEngine::winner_(const ScheduleConfig &config) const {
  for (const auto &entry : this->entries_) {
    if (entry->config.enabled && entry->config.same_target(config))
      return entry.get();
  }
  return nullptr;
}

std::vector<std::string> ScheduleEngine::refresh_() {
  std::vector<std::string> started;
  for (const auto &entry : this->entries_) {
    const bool was = entry->active;
    // A missing target still keeps the others off it: it is the one meant to drive it.
    entry->active = entry->config.enabled && entry->missing == ScheduleState::RUNNING &&
                    this->winner_(entry->config) == entry.get();
    if (entry->active && !was)
      started.push_back(entry->config.id);
  }
  return started;
}

ScheduleStatus ScheduleEngine::status(const std::string &id) const {
  ScheduleStatus status;
  const Entry *entry = this->find_(id);
  if (entry == nullptr || !entry->config.enabled)
    return status;
  if (entry->missing != ScheduleState::RUNNING) {
    status.state = entry->missing;
    // Worded now, with the thermostat's name as it is now.
    switch_::Switch *relay;
    if (this->check_target_(entry->config, &status.reason, &relay) == ScheduleState::RUNNING)
      status.reason = entry->missing_reason;
    return status;
  }
  const Entry *winner = this->winner_(entry->config);
  if (winner != entry) {
    status.state = ScheduleState::CONFLICT;
    status.reason = "Schedule \"" + winner->config.name + "\" drives this target";
    return status;
  }
  if (!this->clock_valid_) {
    status.state = ScheduleState::WAITING_FOR_CLOCK;
    status.reason = "Waiting for the clock";
    return status;
  }
  status.state = ScheduleState::RUNNING;
#ifdef USE_SWITCH
  if (entry->relay != nullptr) {
    const std::string holder = switch_hold::holder(entry->relay);
    if (!holder.empty()) {
      status.state = ScheduleState::HELD;
      status.held_by = holder;
      status.reason = "Thermostat \"" + holder + "\" drives " + entry->relay->get_name().c_str();
    }
  }
#endif
#ifdef USE_CLIMATE_HUB
  if (entry->config.target_type == TargetType::THERMOSTAT && global_climate_hub != nullptr &&
      !global_climate_hub->is_running(entry->config.target_id))
    status.note =
        "Thermostat \"" + this->target_name(entry->config) + "\" is stopped; it starts in the scheduled values";
#endif
  return status;
}

std::string ScheduleEngine::target_name(const ScheduleConfig &config) const {
  if (config.target_type == TargetType::RELAY) {
#ifdef USE_SWITCH
    const switch_::Switch *sw = find_switch(config.target_id);
    if (sw != nullptr)
      return sw->get_name().c_str();
#endif
    return config.target_id;
  }
#ifdef USE_CLIMATE_HUB
  const climate_hub::ClimateConfig *thermostat =
      global_climate_hub != nullptr ? global_climate_hub->store().get(config.target_id) : nullptr;
  if (thermostat != nullptr)
    return thermostat->name;
#endif
  return config.target_id;
}

bool ScheduleEngine::now_and_next(const ScheduleConfig &config, PointRef *now, PointRef *next) const {
  if (!this->clock_valid_)
    return false;
  const uint16_t week = week_minute_of(this->mark_.minute);
  const Point *in_force = config.in_force(week);
  const Point *after = config.next_after(week);
  if (in_force == nullptr || after == nullptr)
    return false;
  auto place = [](const Point *point) {
    PointRef ref;
    ref.day = static_cast<uint8_t>(point->minute_of_week / MINUTES_PER_DAY);
    ref.minute = point->minute_of_week % MINUTES_PER_DAY;
    ref.point = point;
    return ref;
  };
  *now = place(in_force);
  *next = place(after);
  return true;
}

bool ScheduleEngine::clock_text(std::string *day, std::string *time) {
  const ESPTime now = this->clock_now_();
  if (!now.is_valid())
    return false;
  *day = DAY_NAMES[week_minute(now) / MINUTES_PER_DAY];
  char text[16];
  snprintf(text, sizeof(text), "%02u:%02u:%02u", now.hour % 100u, now.minute % 100u, now.second % 100u);
  *time = text;
  return true;
}

std::string ScheduleEngine::scheduled_by(TargetType type, const std::string &id) const {
  ScheduleConfig probe;
  probe.target_type = type;
  probe.target_id = id;
  const Entry *winner = this->winner_(probe);
  return winner == nullptr ? "" : winner->config.id;
}

std::vector<RelayTarget> ScheduleEngine::relay_targets() const {
  std::vector<RelayTarget> out;
#ifdef USE_SWITCH
  for (auto *sw : App.get_switches()) {
    if (sw->is_internal())
      continue;
    char buf[OBJECT_ID_MAX_LEN];
    RelayTarget row;
    row.object_id = sw->get_object_id_to(buf).c_str();
    row.name = sw->get_name().c_str();
    row.held_by = switch_hold::holder(sw);
    row.scheduled_by = this->scheduled_by(TargetType::RELAY, row.object_id);
    out.push_back(std::move(row));
  }
#endif
  return out;
}

std::vector<ThermostatTarget> ScheduleEngine::thermostat_targets() const {
  std::vector<ThermostatTarget> out;
#ifdef USE_CLIMATE_HUB
  if (global_climate_hub == nullptr)
    return out;
  for (const auto &thermostat : global_climate_hub->store().all()) {
    ThermostatTarget row;
    row.id = thermostat->id;
    row.name = thermostat->name;
    row.enabled = thermostat->enabled;
    row.running = global_climate_hub->is_running(thermostat->id);
    row.mode = climate_hub::enums::mode_to_string(thermostat->mode);
    row.min_temperature = thermostat->visual.min_temperature;
    row.max_temperature = thermostat->visual.max_temperature;
    row.step = thermostat->visual.step;
    row.scheduled_by = this->scheduled_by(TargetType::THERMOSTAT, thermostat->id);
    for (const climate_hub::PresetConfig &preset : thermostat->presets) {
      PresetTarget item;
      item.key = preset.key;
      item.name = preset.name;
      item.setpoint = preset.setpoint;
      item.mode =
          preset.mode.has_value() ? climate_hub::enums::mode_to_string(*preset.mode) : climate_hub::PRESET_MODE_KEEP;
      row.presets.push_back(std::move(item));
    }
    out.push_back(std::move(row));
  }
#endif
  return out;
}

bool ScheduleEngine::is_name_taken(const std::string &name, const std::string &exclude_id, std::string *error) const {
  const std::string key = name_key(name);
  for (const auto &entry : this->entries_) {
    if (entry->config.id != exclude_id && name_key(entry->config.name) == key) {
      if (error != nullptr)
        *error = "\"" + name + "\" is already used by another schedule";
      return true;
    }
  }
  return false;
}

// --- Mutators ---

bool ScheduleEngine::refuse_if_failed_(Result *result) const {
  if (!this->is_failed())
    return false;
  *result = failure(500, "Schedule storage is not available");
  return true;
}

bool ScheduleEngine::check_target_free_(const ScheduleConfig &doc, const std::string &self_id, Result *result) const {
  const Entry *winner = this->winner_(doc);
  if (winner == nullptr || winner->config.id == self_id)
    return true;
  *result = failure(409, "\"" + winner->config.name + "\" already drives " + this->target_name(doc) +
                             "; switch it off first or take over");
  result->holder = winner->config.id;
  return false;
}

Result ScheduleEngine::create(ScheduleConfig draft) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  std::string error;
  draft.name = trim_name(draft.name);
  draft.sort_points();
  if (!draft.validate(&error))
    return failure(400, error);
  if (this->entries_.size() >= this->max_schedules_)
    return failure(
        507, "This device allows " + std::to_string(this->max_schedules_) + " schedules; delete one to add another");
  if (this->is_name_taken(draft.name, "", &error))
    return failure(409, error);
  draft.id = this->next_id_(draft.name);
  if (draft.id.empty())
    return failure(409, "Every id made from \"" + draft.name +
                            "\" is taken by a file in the schedule folder; choose another name");
  if (draft.enabled && !this->check_target_free_(draft, draft.id, &result))
    return result;
  draft.version = SCHEDULE_VERSION;
  bool too_large = false;
  if (!this->save_(draft, &too_large))
    return not_saved(too_large);
  return this->add_(std::move(draft), "Created");
}

Result ScheduleEngine::update(const std::string &id, ScheduleConfig doc) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  Entry *stored = this->find_(id);
  if (stored == nullptr)
    return failure(404, NOT_FOUND);
  if (stored->config.from_newer_firmware())
    return failure(409, NEWER_FILE);
  std::string error;
  doc.name = trim_name(doc.name);
  doc.sort_points();
  if (!doc.validate(&error))
    return failure(400, error);
  if (this->is_name_taken(doc.name, id, &error))
    return failure(409, error);
  // The path wins: a Save never re-keys a schedule.
  doc.id = id;
  if (doc.enabled && !this->check_target_free_(doc, id, &result))
    return result;
  doc.version = SCHEDULE_VERSION;
  // Written beside the old file and renamed over it: a failure leaves everything as it was.
  bool too_large = false;
  if (!this->save_(doc, &too_large))
    return not_saved(too_large);
  return this->replace_(stored, std::move(doc), "Updated");
}

Result ScheduleEngine::restore(ScheduleConfig doc) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  std::string error;
  if (!validate_id(doc.id, &error))
    return failure(400, error);
  // The loader refuses such a file, so the schedule would be gone after the next boot.
  if (doc.id == RESERVED_ID)
    return failure(400, "id 'new' is reserved");
  doc.name = trim_name(doc.name);
  doc.sort_points();
  if (!doc.validate(&error))
    return failure(400, error);
  Entry *stored = this->find_(doc.id);
  if (stored != nullptr && stored->config.from_newer_firmware())
    return failure(409, NEWER_FILE);
  if (stored == nullptr && this->entries_.size() >= this->max_schedules_)
    return failure(
        507, "This device allows " + std::to_string(this->max_schedules_) + " schedules; delete one to add another");
  if (this->is_name_taken(doc.name, doc.id, &error))
    return failure(409, error);
  // As at create: a file the loader refused is the only record of what its author meant.
  if (stored == nullptr && file_exists(this->file_path_(doc.id)))
    return failure(409, "The id \"" + doc.id + "\" is taken by a file in the schedule folder that was not loaded");
  if (doc.enabled && !this->check_target_free_(doc, doc.id, &result))
    return result;
  doc.version = SCHEDULE_VERSION;
  bool too_large = false;
  if (!this->save_(doc, &too_large))
    return not_saved(too_large);
  if (stored == nullptr)
    return this->add_(std::move(doc), "Restored");
  return this->replace_(stored, std::move(doc), "Restored");
}

Result ScheduleEngine::add_(ScheduleConfig doc, const char *verb) {
  auto entry = std::make_unique<Entry>();
  entry->config = std::move(doc);
  const std::string id = entry->config.id;
  ESP_LOGD(TAG, "%s '%s' (%s)", verb, entry->config.name.c_str(), id.c_str());
  this->resolve_(*entry);
  auto at =
      std::upper_bound(this->entries_.begin(), this->entries_.end(), id,
                       [](const std::string &key, const std::unique_ptr<Entry> &e) { return key < e->config.id; });
  this->entries_.insert(at, std::move(entry));
  Result result = success();
  result.id = id;
  this->settle_(id, true, &result);
  return result;
}

Result ScheduleEngine::replace_(Entry *stored, ScheduleConfig doc, const char *verb) {
  stored->config = std::move(doc);
  this->resolve_(*stored);
  const std::string id = stored->config.id;
  ESP_LOGD(TAG, "%s '%s' (%s)", verb, stored->config.name.c_str(), id.c_str());
  Result result = success();
  result.id = id;
  this->settle_(id, true, &result);
  return result;
}

void ScheduleEngine::settle_(const std::string &id, bool save, Result *result) {
  const std::vector<std::string> started = this->refresh_();
  const Entry *own = this->find_(id);
  if (own != nullptr && own->config.enabled && own->missing != ScheduleState::RUNNING) {
    result->warning = "not running: " + lower_first(own->missing_reason);
    ESP_LOGW(TAG, "'%s' saved but %s", id.c_str(), result->warning.c_str());
  }
  const bool own_started = std::find(started.begin(), started.end(), id) != started.end();
  if (own != nullptr && ((save && own->config.enabled) || own_started))
    this->start_(id, save ? "saved" : "enabled");
  for (const std::string &other : started) {
    if (other == id)
      continue;
    ESP_LOGI(TAG, "'%s' drives its target now", other.c_str());
    this->start_(other, "target free");
    result->started.push_back(other);
  }
}

Result ScheduleEngine::remove(const std::string &id) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  if (this->find_(id) == nullptr)
    return failure(404, NOT_FOUND);
  // `id` may be the document's own string, which the removal frees.
  const std::string removed = id;
  result = success();
  result.persisted = this->delete_file_(removed);
  this->entries_.erase(std::find_if(this->entries_.begin(), this->entries_.end(),
                                    [&removed](const std::unique_ptr<Entry> &e) { return e->config.id == removed; }));
  ESP_LOGD(TAG, "Removed '%s'", removed.c_str());
  this->settle_(removed, false, &result);
  return result;
}

Result ScheduleEngine::set_enabled(const std::string &id, bool enabled, bool take_over) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  Entry *stored = this->find_(id);
  if (stored == nullptr)
    return failure(404, NOT_FOUND);
  result = success();
  if (!enabled) {
    if (stored->config.enabled) {
      stored->config.enabled = false;
      result.persisted = this->save_(stored->config);
    }
    this->settle_(id, false, &result);
    return result;
  }
  if (!this->check_target_free_(stored->config, id, &result)) {
    if (!take_over)
      return result;
    result = success();
    // A newer firmware's file cannot record the take-over, so the others' files do not either:
    // the next boot runs what the files say rather than neither.
    const bool in_memory = stored->config.from_newer_firmware();
    for (const auto &other : this->entries_) {
      if (other.get() == stored || !other->config.enabled || !other->config.same_target(stored->config))
        continue;
      other->config.enabled = false;
      result.persisted = !in_memory && this->save_(other->config) && result.persisted;
      ESP_LOGI(TAG, "'%s' took the target over from '%s'", id.c_str(), other->config.id.c_str());
      result.stopped.push_back(other->config.id);
    }
  }
  if (!stored->config.enabled) {
    stored->config.enabled = true;
    result.persisted = this->save_(stored->config) && result.persisted;
  }
  this->settle_(id, false, &result);
  return result;
}

// --- Files ---

std::string ScheduleEngine::folder_() const { return this->storage_->get_base_path() + "/" + this->folder_path_; }

std::string ScheduleEngine::file_path_(const std::string &id) const { return this->folder_() + "/" + id + ".json"; }

bool ScheduleEngine::ensure_folder_() {
  const std::string path = this->folder_();
  struct stat st;
  if (stat(path.c_str(), &st) == 0)
    return S_ISDIR(st.st_mode);
  return mkdir(path.c_str(), 0755) == 0;
}

// A file the loader refused still holds its id: a new schedule never writes over it.
std::string ScheduleEngine::next_id_(const std::string &name) const {
  const std::string base = slugify_id(name);
  for (unsigned n = 1; n <= MAX_ID_SUFFIX; n++) {
    const std::string candidate = id_with_suffix(base, n);
    if (candidate == RESERVED_ID)
      continue;
    if (this->find_(candidate) == nullptr && !file_exists(this->file_path_(candidate)))
      return candidate;
  }
  return "";
}

bool ScheduleEngine::save_(const ScheduleConfig &config, bool *too_large) {
  if (too_large != nullptr)
    *too_large = false;
  if (config.from_newer_firmware()) {
    ESP_LOGW(TAG, "'%s' not written: a newer firmware wrote its file", config.id.c_str());
    return false;
  }
  std::string json;
  const EncodeError encoded = this->encode_(config, &json);
  if (encoded == EncodeError::TOO_LARGE) {
    ESP_LOGE(TAG, "'%s' not written: over %u bytes", config.id.c_str(), static_cast<unsigned>(MAX_FILE_BYTES));
    if (too_large != nullptr)
      *too_large = true;
    return false;
  }
  if (encoded == EncodeError::NO_MEMORY) {
    ESP_LOGE(TAG, "'%s' not written: out of memory", config.id.c_str());
    return false;
  }
  if (!this->ensure_folder_()) {
    ESP_LOGE(TAG, "Cannot create '%s'", this->folder_().c_str());
    return false;
  }
  // Written beside the target and renamed over it: a write that fails or loses power leaves
  // the old file whole. The close is where a full filesystem shows up.
  const std::string path = this->file_path_(config.id);
  const std::string tmp = path + ".tmp";
  FILE *file = fopen(tmp.c_str(), "w");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Cannot write '%s'", tmp.c_str());
    return false;
  }
  const size_t written = fwrite(json.data(), 1, json.size(), file);
  const bool closed = fclose(file) == 0;
  if (written != json.size() || !closed || rename(tmp.c_str(), path.c_str()) != 0) {
    ESP_LOGE(TAG, "Cannot write '%s'", path.c_str());
    ::remove(tmp.c_str());
    return false;
  }
  return true;
}

bool ScheduleEngine::remove_file_(const std::string &path) { return ::remove(path.c_str()) == 0; }

bool ScheduleEngine::delete_file_(const std::string &id) {
  const std::string path = this->file_path_(id);
  if (this->remove_file_(path) || !file_exists(path))
    return true;
  // Left whole, the loader would bring the schedule back at the next boot; blank, it refuses it.
  ESP_LOGW(TAG, "Cannot delete '%s'; blanking it", path.c_str());
  FILE *blank = fopen(path.c_str(), "w");
  if (blank != nullptr && fclose(blank) == 0)
    return true;
  ESP_LOGE(TAG, "Cannot blank '%s': it comes back at the next boot", path.c_str());
  return false;
}

}  // namespace esphome::schedules
