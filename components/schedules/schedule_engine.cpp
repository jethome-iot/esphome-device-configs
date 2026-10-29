#include "schedule_engine.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#ifdef USE_ESP32
#include <esp_heap_caps.h>
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
// A form read before another change would undo it.
static const char *const STALE_DOCUMENT = "The device changed this schedule since it was read; reload it";
// The dashboard's editor opens a blank form at /schedules/new.
static const char *const RESERVED_ID = "new";
static const char *const NO_THERMOSTATS = "This device has no thermostats";

const char *const NO_MEMORY_TO_SAVE = "Not enough free memory on the device to save this schedule";

const char *state_to_string(ScheduleState state) {
  switch (state) {
    case ScheduleState::DISABLED:
      return "disabled";
    case ScheduleState::WAITING_FOR_CLOCK:
      return "waiting_for_clock";
    case ScheduleState::CONFLICT:
      return "conflict";
    case ScheduleState::RUNNING:
      return "running";
  }
  return "running";
}

const char *issue_kind_to_string(IssueKind kind) {
  switch (kind) {
    case IssueKind::MISSING_RELAY:
      return "missing_relay";
    case IssueKind::MISSING_THERMOSTAT:
      return "missing_thermostat";
    case IssueKind::MISSING_PRESET:
      return "missing_preset";
    case IssueKind::NO_THERMOSTATS:
      return "no_thermostats";
    case IssueKind::HELD:
      return "held";
  }
  return "held";
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

// A fake heap may say SIZE_MAX.
static size_t saturating_add(size_t a, size_t b) { return a > SIZE_MAX - b ? SIZE_MAX : a + b; }

static Result success() {
  Result result;
  result.ok = true;
  return result;
}

// A create or update whose file was not written: nothing changed.
static Result not_saved(EncodeError why) {
  if (why == EncodeError::TOO_LARGE)
    return failure(413, "The schedule's file would be over 16 KiB");
  if (why == EncodeError::NO_MEMORY)
    return failure(507, NO_MEMORY_TO_SAVE);
  Result result = failure(500, "The schedule's file could not be written");
  result.persisted = false;
  return result;
}

static_assert(MAX_FILE_BYTES == 16384, "the sentence above names the limit");

// ArduinoJson's reader over a file through a small buffer: a file is parsed as it is read,
// never held whole.
class FileReader {
 public:
  explicit FileReader(FILE *file) : file_(file) {}
  int read() {
    if (this->next_ == this->end_ && !this->fill_())
      return -1;
    return static_cast<uint8_t>(this->buffer_[this->next_++]);
  }
  size_t readBytes(char *out, size_t length) {
    size_t got = 0;
    while (got < length && (this->next_ < this->end_ || this->fill_())) {
      const size_t n = std::min(length - got, this->end_ - this->next_);
      memcpy(out + got, this->buffer_ + this->next_, n);
      this->next_ += n;
      got += n;
    }
    return got;
  }

 protected:
  bool fill_() {
    this->next_ = 0;
    this->end_ = fread(this->buffer_, 1, sizeof(this->buffer_), this->file_);
    return this->end_ > 0;
  }
  FILE *file_;
  char buffer_[256];
  size_t next_{0};
  size_t end_{0};
};

// ArduinoJson's writer into a file through a small buffer, so no file is ever one block of RAM.
class FileWriter {
 public:
  explicit FileWriter(FILE *file) : file_(file) {}
  size_t write(uint8_t c) { return this->write(&c, 1); }
  size_t write(const uint8_t *data, size_t length) {
    for (size_t done = 0; done < length;) {
      const size_t n = std::min(length - done, sizeof(this->buffer_) - this->used_);
      memcpy(this->buffer_ + this->used_, data + done, n);
      this->used_ += n;
      done += n;
      if (this->used_ == sizeof(this->buffer_))
        this->flush();
    }
    return length;
  }
  /// False once any write failed.
  bool flush() {
    if (this->used_ > 0 && fwrite(this->buffer_, 1, this->used_, this->file_) != this->used_)
      this->failed_ = true;
    this->used_ = 0;
    return !this->failed_;
  }

 protected:
  FILE *file_;
  uint8_t buffer_[256];
  size_t used_{0};
  bool failed_{false};
};

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

// A reason as the tail of a warning: "...: relay ...".
static std::string lower_first(std::string text) {
  if (!text.empty() && text[0] >= 'A' && text[0] <= 'Z')
    text[0] = static_cast<char>(text[0] - 'A' + 'a');
  return text;
}

// For the log: "on", "preset 'eco'", "target 21.5".
static std::string describe(const ScheduleConfig &config, const Action &action) {
  switch (action.value) {
    case ValueType::PRESET:
      return "preset '" + std::string(config.preset_of(action)) + "'";
    case ValueType::TARGET: {
      char text[64];
      snprintf(text, sizeof(text), "target %.1f", action.number);
      return text;
    }
    default:
      return value_type_to_string(action.value);
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
  // Nothing runs before the first tick that reads a valid clock.
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
    ESP_LOGW(TAG, "Clock moved by %+" PRId64 " s: running the modes in force", drift);
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
  std::vector<std::pair<std::string, uint8_t>> due;
  for (const auto &entry : this->entries_) {
    const Point *point = entry->active ? entry->config.in_force(week) : nullptr;
    if (point == nullptr)
      continue;
    const int64_t back = (week - point->minute_of_week + MINUTES_PER_WEEK) % MINUTES_PER_WEEK;
    if (minute - back > from)
      due.emplace_back(entry->config.id, point->mode);
  }
  for (const auto &item : due)
    this->run_(item.first, item.second, "switch point");
}

void ScheduleEngine::start_all_(const char *why) {
  std::vector<std::string> ids;
  for (const auto &entry : this->entries_)
    ids.emplace_back(entry->config.id);
  for (const std::string &id : ids)
    this->start_(id, why);
}

void ScheduleEngine::start_(const std::string &id, const char *why) {
  const Entry *entry = this->find_(id);
  if (entry == nullptr || !entry->active || !this->clock_valid_)
    return;
  const Point *point = entry->config.in_force(week_minute_of(this->mark_.minute));
  if (point != nullptr)
    this->run_(id, point->mode, why);
}

// --- Running a list ---

void ScheduleEngine::cancel_(Entry &entry) {
  if (entry.waiting)
    this->cancel_wait_(entry.timer);
  entry.waiting = false;
  entry.mode = -1;
  entry.next = 0;
  entry.lost.clear();
  entry.run = this->next_run_++;
}

void ScheduleEngine::run_(const std::string &id, uint8_t mode, const char *why) {
  Entry *entry = this->find_(id);
  if (entry == nullptr || !entry->active || mode >= entry->config.modes.size())
    return;
  // A new point stops a list still in a wait.
  this->cancel_(*entry);
  entry->mode = static_cast<int8_t>(mode);
  ESP_LOGI(TAG, "Schedule '%s' (%s): mode '%s'", entry->config.name.c_str(), why,
           entry->config.modes[mode].name.c_str());
  this->advance_(std::string(entry->config.id));
}

void ScheduleEngine::advance_(std::string id) {
  const Entry *first = this->find_(id);
  if (first == nullptr)
    return;
  const uint32_t run = first->run;
  for (;;) {
    // Looked up again after every action: what it set off may have changed or removed it.
    Entry *entry = this->find_(id);
    if (entry == nullptr || entry->run != run || !entry->active || entry->mode < 0)
      return;
    const Mode &mode = entry->config.modes[entry->mode];
    if (entry->next >= mode.count)
      return;
    const auto index = entry->next++;
    const Action action = entry->config.action(mode, index);
    if (action.type == ActionType::WAIT) {
      entry->waiting = true;
      const auto ms = static_cast<uint32_t>(action.number) * 1000u;
      this->wait_(entry->timer, ms, [this, id, run]() {
        Entry *waited = this->find_(id);
        if (waited == nullptr || waited->run != run)
          return;
        waited->waiting = false;
        this->advance_(id);
      });
      return;
    }
    if (action.type == ActionType::RELAY) {
      this->apply_relay_(*entry, action);
    } else {
      this->apply_thermostat_(*entry, action, index);
    }
  }
}

void ScheduleEngine::mark_lost_(const std::string &id, uint32_t run, uint8_t index) {
  Entry *entry = this->find_(id);
  if (entry == nullptr || entry->run != run)
    return;
  if (std::find(entry->lost.begin(), entry->lost.end(), index) == entry->lost.end())
    entry->lost.push_back(index);
}

void ScheduleEngine::apply_relay_(Entry &entry, const Action &action) {
  switch_::Switch *sw = entry.switches[action.target];
  if (sw == nullptr) {
    ESP_LOGW(TAG, "Schedule '%s': relay '%s' is not on this device; skipped", entry.config.name.c_str(),
             entry.config.target_of(action).id.c_str());
    return;
  }
#ifdef USE_SWITCH
  // A running thermostat's relay is the thermostat's: it follows the list once it is let go.
  const std::string holder = switch_hold::holder(sw);
  if (!holder.empty()) {
    ESP_LOGI(TAG, "Schedule '%s' left '%s' alone: thermostat '%s' drives it", entry.config.name.c_str(),
             sw->get_name().c_str(), holder.c_str());
    return;
  }
  ESP_LOGI(TAG, "Schedule '%s': '%s' %s", entry.config.name.c_str(), sw->get_name().c_str(),
           describe(entry.config, action).c_str());
  // Last: the switch's callbacks may change anything, `entry` included.
  if (action.value == ValueType::ON) {
    sw->turn_on();
  } else {
    sw->turn_off();
  }
#endif
}

bool ScheduleEngine::thermostat_ready_(const std::string &thermostat, const std::string &key) const {
#ifdef USE_CLIMATE_HUB
  const climate_hub::ClimateConfig *found =
      global_climate_hub != nullptr ? global_climate_hub->store().get(thermostat) : nullptr;
  return found != nullptr && (key.empty() || found->find_preset(key) != nullptr);
#else
  (void) thermostat;
  (void) key;
  return false;
#endif
}

// The calls an automation rule makes, so the hub treats it the same: a stopped thermostat's
// document takes the values and stays stopped, a relay it holds and a calibration as for a rule.
void ScheduleEngine::apply_thermostat_(Entry &entry, const Action &action, uint8_t index) {
  // Copies: the hub's calls below may change the entry.
  const std::string id(entry.config.id);
  const std::string name(entry.config.name);
  const std::string target(entry.config.target_of(action).id);
  const std::string key(entry.config.preset_of(action));
  const uint32_t run = entry.run;
#ifdef USE_CLIMATE_HUB
  climate_hub::ClimateHub *hub = global_climate_hub;
  if (!this->thermostat_ready_(target, key)) {
    const bool there = hub != nullptr && hub->store().get(target) != nullptr;
    ESP_LOGW(TAG, "Schedule '%s': thermostat '%s' %s; skipped", name.c_str(), target.c_str(),
             there ? ("has no preset '" + key + "'").c_str() : "is not on this device");
    this->mark_lost_(id, run, index);
    return;
  }
  const climate_hub::ClimateConfig *thermostat = hub->store().get(target);
  ESP_LOGI(TAG, "Schedule '%s': thermostat '%s' %s", name.c_str(), target.c_str(),
           describe(entry.config, action).c_str());
  climate_hub::Result result;
  if (action.value == ValueType::OFF) {
    result = hub->turn_off(target);
  } else if (action.value == ValueType::ON) {
    result = hub->turn_on(target);
  } else if (action.value == ValueType::PRESET) {
    const climate_hub::PresetConfig *preset = thermostat->find_preset(key);
    // A preset that keeps the mode would leave an off thermostat off.
    const bool keeps_mode = !(preset->mode.has_value() && thermostat->supports_mode(*preset->mode));
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
      result = hub->set_setpoint(target, action.number);
  }
  if (result.ok)
    return;
  // The hub dropped the thermostat or the preset without saying so: it applies again once back.
  ESP_LOGW(TAG, "Schedule '%s': thermostat '%s': %s", name.c_str(), target.c_str(), result.error.c_str());
  this->mark_lost_(id, run, index);
#else
  ESP_LOGW(TAG, "Schedule '%s': thermostat '%s' skipped: %s", name.c_str(), target.c_str(), NO_THERMOSTATS);
  this->mark_lost_(id, run, index);
#endif
}

void ScheduleEngine::on_climate_change_(const std::string &climate_id) {
  const Target probe{TargetType::THERMOSTAT, climate_id};
  for (auto &entry : this->entries_) {
    if (!entry->active || entry->mode < 0)
      continue;
    const auto &targets = entry->config.targets;
    const auto found = std::find(targets.begin(), targets.end(), probe);
    if (found == targets.end())
      continue;
    const auto target = static_cast<uint8_t>(found - targets.begin());
    // The actions on it the list passed: whether one it lost can go in now, and which cannot.
    const Mode &mode = entry->config.modes[entry->mode];
    bool back = false;
    PsramVector<uint8_t> lost;
    for (const uint8_t index : entry->lost) {
      const Action &action = entry->config.action(mode, index);
      if (action.target != target)
        lost.push_back(index);
    }
    for (size_t i = 0; i < entry->next && i < mode.count; i++) {
      const Action &action = entry->config.action(mode, i);
      if (action.type != ActionType::THERMOSTAT || action.target != target)
        continue;
      const auto index = static_cast<uint8_t>(i);
      const bool was_lost = std::find(entry->lost.begin(), entry->lost.end(), index) != entry->lost.end();
      if (!this->thermostat_ready_(climate_id, std::string(entry->config.preset_of(action)))) {
        lost.push_back(index);
      } else if (was_lost) {
        back = true;
      }
    }
    entry->lost = std::move(lost);
    if (!back)
      continue;
    // From inside the hub's own change: the values go in once the loop calls the hub again.
    const std::string id(entry->config.id);
    const uint32_t run = entry->run;
    this->later_([this, id, run, target]() { this->reapply_(id, run, target); });
  }
}

void ScheduleEngine::reapply_(const std::string &id, uint32_t run, uint8_t target) {
  for (size_t i = 0;; i++) {
    Entry *entry = this->find_(id);
    if (entry == nullptr || entry->run != run || !entry->active || entry->mode < 0)
      return;
    const Mode &mode = entry->config.modes[entry->mode];
    if (i >= entry->next || i >= mode.count)
      return;
    const Action action = entry->config.action(mode, i);
    if (action.type == ActionType::THERMOSTAT && action.target == target)
      this->apply_thermostat_(*entry, action, static_cast<uint8_t>(i));
  }
}

void ScheduleEngine::on_released_(switch_::Switch *sw) {
  for (const auto &entry : this->entries_) {
    if (!entry->active || entry->mode < 0)
      continue;
    for (size_t t = 0; t < entry->switches.size(); t++) {
      if (entry->switches[t] != sw)
        continue;
      const std::string id(entry->config.id);
      const uint32_t run = entry->run;
      const auto target = static_cast<uint8_t>(t);
      // Inside the holder's own change: the relay moves once that is done.
      this->later_([this, id, run, target]() { this->follow_relay_(id, run, target); });
    }
  }
}

void ScheduleEngine::follow_relay_(const std::string &id, uint32_t run, uint8_t target) {
  Entry *entry = this->find_(id);
  if (entry == nullptr || entry->run != run || !entry->active || entry->mode < 0)
    return;
  const Mode &mode = entry->config.modes[entry->mode];
  // The last action on it the list has passed; none yet, and the list gets to it itself.
  const Action *last = nullptr;
  for (size_t i = 0; i < entry->next && i < mode.count; i++) {
    const Action &action = entry->config.action(mode, i);
    if (action.type == ActionType::RELAY && action.target == target)
      last = &action;
  }
  if (last != nullptr) {
    const Action action = *last;
    this->apply_relay_(*entry, action);
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
  this->left_for_heap_ = 0;
  for (const std::string &name : names) {
    if (this->entries_.size() >= this->max_schedules_) {
      ESP_LOGW(TAG, "'%s' left alone: at most %u schedules", name.c_str(), static_cast<unsigned>(this->max_schedules_));
      continue;
    }
    // Once the heap ran short the rest stay on flash too, so which ones run does not depend on size.
    if (this->left_for_heap_ > 0) {
      ESP_LOGW(TAG, "'%s' left alone: not enough free memory", name.c_str());
      this->left_for_heap_++;
      continue;
    }
    // setup() runs before the loop feeds the watchdog.
    App.feed_wdt();
    if (this->load_file_(folder, name) == Load::NO_MEMORY) {
      ESP_LOGW(TAG, "'%s' left alone: not enough free memory (%s)", name.c_str(), this->room_text_().c_str());
      this->left_for_heap_ = 1;
    }
  }
  for (auto &entry : this->entries_)
    this->resolve_(*entry);
  this->refresh_();
  ESP_LOGD(TAG, "Loaded %u schedules from '%s'; heap %s", static_cast<unsigned>(this->entries_.size()), folder.c_str(),
           this->room_text_().c_str());
  return true;
}

// At boot nothing else allocates yet, so the heaps show what the schedules took.
bool ScheduleEngine::boot_room_() const {
  const HeapRoom heap = this->heap_();
  if (heap.free < BOOT_HEAP_FLOOR_BYTES || heap.largest_block < BOOT_HEAP_FLOOR_BLOCK)
    return false;
  const optional<HeapRoom> psram = this->psram_();
  return !psram.has_value() || (psram->free >= PSRAM_RESERVE_BYTES && psram->largest_block >= PSRAM_RESERVE_BLOCK);
}

// A file that is refused stays exactly as it is: the folder is writable by hand and over the
// file API, and the file is the only record of what its author meant.
ScheduleEngine::Load ScheduleEngine::load_file_(const std::string &folder, const std::string &filename) {
  if (!this->boot_room_())
    return Load::NO_MEMORY;
  const std::string path = folder + "/" + filename;
  const std::string stem = filename.substr(0, filename.size() - 5);
  FILE *file = fopen(path.c_str(), "r");
  if (file == nullptr) {
    ESP_LOGW(TAG, "Cannot open '%s'", path.c_str());
    return Load::REFUSED;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size <= 0 || static_cast<size_t>(size) > MAX_FILE_BYTES) {
    ESP_LOGW(TAG, "'%s' refused: %ld bytes", path.c_str(), size);
    fclose(file);
    return Load::REFUSED;
  }
  auto entry = std::make_unique<Entry>();
  ScheduleConfig &config = entry->config;
  {
    JsonDocument doc(this->json_allocator_());
    FileReader reader(file);
    const DeserializationError parsed = deserializeJson(doc, reader);
    fclose(file);
    if (parsed == DeserializationError::NoMemory)
      return Load::NO_MEMORY;
    if (parsed) {
      ESP_LOGE(TAG, "'%s' refused: not valid JSON", path.c_str());
      return Load::REFUSED;
    }
    std::string error;
    if (!config.deserialize(doc.as<JsonObjectConst>(), true, &error)) {
      ESP_LOGE(TAG, "'%s' refused: %s", path.c_str(), error.c_str());
      return Load::REFUSED;
    }
  }
  // What the schedule keeps, the parsed file gone.
  if (!this->boot_room_())
    return Load::NO_MEMORY;
  if (config.id != stem) {
    ESP_LOGE(TAG, "'%s' refused: its id '%s' is not its file name", path.c_str(), config.id.c_str());
    return Load::REFUSED;
  }
  if (config.id == RESERVED_ID) {
    ESP_LOGE(TAG, "'%s' refused: the editor cannot open the id '%s'", path.c_str(), RESERVED_ID);
    return Load::REFUSED;
  }
  if (config.from_newer_firmware()) {
    ESP_LOGW(TAG, "'%s' is version %u, from a newer firmware: it runs, but changes stay in memory", path.c_str(),
             static_cast<unsigned>(config.version));
  }
  this->resolve_name_(&config);
  entry->timer = this->next_timer_++;
  entry->run = this->next_run_++;
  // Loaded in file name order, which is id order.
  this->entries_.push_back(std::move(entry));
  return Load::LOADED;
}

// Two documents under one name: the later one by id becomes "<name> 2" and is written back, so
// the next boot agrees.
void ScheduleEngine::resolve_name_(ScheduleConfig *config) {
  if (!this->is_name_taken(config->name, config->id, nullptr))
    return;
  const std::string original(config->name);
  // One of max_schedules + 1 candidates is free whatever the others are called.
  std::string candidate;
  for (unsigned n = 2; candidate.empty() || this->is_name_taken(candidate, config->id, nullptr); n++) {
    const std::string tail = " " + std::to_string(n);
    candidate = trim_name(cut_chars(original, NAME_MAX_LENGTH - tail.size())) + tail;
  }
  config->name = candidate;
  config->revision++;
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
  if (this->left_for_heap_ > 0)
    ESP_LOGCONFIG(TAG, "  Left on flash for lack of memory: %u", static_cast<unsigned>(this->left_for_heap_));
  for (const auto &entry : this->entries_) {
    const ScheduleStatus status = this->status(entry->config.id);
    ESP_LOGCONFIG(TAG, "  '%s' (%s): %u modes, %u targets, %s%s%s%s", entry->config.name.c_str(),
                  entry->config.id.c_str(), static_cast<unsigned>(entry->config.modes.size()),
                  static_cast<unsigned>(entry->config.targets.size()), state_to_string(status.state),
                  status.reason.empty() ? "" : ": ", status.reason.c_str(),
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

ScheduleEngine::Entry *ScheduleEngine::find_(std::string_view id) const {
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

const ScheduleConfig *ScheduleEngine::get(std::string_view id) const {
  const Entry *entry = this->find_(id);
  return entry == nullptr ? nullptr : &entry->config;
}

bool ScheduleEngine::document(const std::string &id, JsonDocument &doc) const {
  const Entry *entry = this->find_(id);
  if (entry == nullptr)
    return false;
  doc.clear();
  entry->config.serialize(doc.to<JsonObject>());
  return true;
}

// `more` lands where a document does: in PSRAM, or on the internal heap without it.
bool ScheduleEngine::has_heap_room(size_t more) const {
  const HeapRoom heap = this->heap_();
  const optional<HeapRoom> psram = this->psram_();
  const size_t internal_more = psram.has_value() ? 0 : more;
  if (heap.free < HEAP_RESERVE_BYTES + internal_more || heap.largest_block < HEAP_RESERVE_BLOCK)
    return false;
  return !psram.has_value() ||
         (psram->free >= PSRAM_RESERVE_BYTES + more && psram->largest_block >= PSRAM_RESERVE_BLOCK);
}

HeapRoom ScheduleEngine::heap_() const {
  HeapRoom heap;
#ifdef USE_ESP32
  // What the debug component reports, and what the network stack draws on.
  heap.free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  heap.largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
#else
  heap.free = SIZE_MAX;
  heap.largest_block = SIZE_MAX;
#endif
  return heap;
}

optional<HeapRoom> ScheduleEngine::psram_() const {
#ifdef USE_ESP32
  // Where RAMAllocator puts what it can; a board whose PSRAM did not come up has none.
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
    HeapRoom psram;
    psram.free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    psram.largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    return psram;
  }
#endif
  return nullopt;
}

std::string ScheduleEngine::room_text_() const {
  const HeapRoom heap = this->heap_();
  char text[112];
  int n = snprintf(text, sizeof(text), "%u bytes free, %u in one block", static_cast<unsigned>(heap.free),
                   static_cast<unsigned>(heap.largest_block));
  const optional<HeapRoom> psram = this->psram_();
  if (psram.has_value() && n > 0 && static_cast<size_t>(n) < sizeof(text)) {
    snprintf(text + n, sizeof(text) - n, "; PSRAM %u free, %u in one block", static_cast<unsigned>(psram->free),
             static_cast<unsigned>(psram->largest_block));
  }
  return text;
}

void ScheduleEngine::resolve_(Entry &entry) {
  entry.switches.clear();
  for (const Target &target : entry.config.targets)
    entry.switches.push_back(target.type == TargetType::RELAY ? find_switch(std::string(target.id)) : nullptr);
}

const ScheduleEngine::Entry *ScheduleEngine::holder_(const Target &target) const {
  for (const auto &entry : this->entries_) {
    if (entry->active && entry->config.names_target(target))
      return entry.get();
  }
  return nullptr;
}

std::vector<std::string> ScheduleEngine::refresh_() {
  std::vector<std::string> started;
  // By id: each enabled one runs unless one before it that runs drives a target of it. One that
  // waits drives nothing, so the next can have its other targets.
  std::vector<const Target *> driven;
  for (const auto &entry : this->entries_) {
    const bool was = entry->active;
    const auto &targets = entry->config.targets;
    entry->active = entry->config.enabled && std::none_of(targets.begin(), targets.end(), [&driven](const Target &t) {
                      return std::any_of(driven.begin(), driven.end(), [&t](const Target *d) { return *d == t; });
                    });
    // A target that is not there still keeps the others off it: it is the one meant to drive it.
    if (entry->active) {
      for (const Target &target : targets)
        driven.push_back(&target);
    }
    if (was && !entry->active)
      this->cancel_(*entry);
    if (entry->active && !was)
      started.emplace_back(entry->config.id);
  }
  return started;
}

std::vector<Issue> ScheduleEngine::issues_(const ScheduleConfig &config,
                                           const PsramVector<switch_::Switch *> &switches) const {
  std::vector<Issue> issues;
  for (size_t m = 0; m < config.modes.size(); m++) {
    const Mode &mode = config.modes[m];
    for (size_t i = 0; i < mode.count; i++) {
      const Action &action = config.action(mode, i);
      if (!action.drives())
        continue;
      Issue issue;
      issue.mode = static_cast<uint8_t>(m);
      issue.action = static_cast<uint8_t>(i);
      const std::string id(config.target_of(action).id);
      if (action.type == ActionType::RELAY) {
        switch_::Switch *sw = action.target < switches.size() ? switches[action.target] : nullptr;
        if (sw == nullptr) {
          issue.kind = IssueKind::MISSING_RELAY;
          issue.text = "Relay \"" + id + "\" is not on this device";
          issues.push_back(std::move(issue));
          continue;
        }
#ifdef USE_SWITCH
        issue.held_by = switch_hold::holder(sw);
        if (!issue.held_by.empty()) {
          issue.kind = IssueKind::HELD;
          issue.text = "Thermostat \"" + issue.held_by + "\" drives " + sw->get_name().c_str();
          issues.push_back(std::move(issue));
        }
#endif
        continue;
      }
#ifdef USE_CLIMATE_HUB
      if (global_climate_hub != nullptr) {
        const climate_hub::ClimateConfig *thermostat = global_climate_hub->store().get(id);
        if (thermostat == nullptr) {
          issue.kind = IssueKind::MISSING_THERMOSTAT;
          issue.text = "Thermostat \"" + id + "\" is not on this device";
          issues.push_back(std::move(issue));
        } else if (action.value == ValueType::PRESET &&
                   thermostat->find_preset(std::string(config.preset_of(action))) == nullptr) {
          issue.kind = IssueKind::MISSING_PRESET;
          issue.text =
              "Thermostat \"" + thermostat->name + "\" has no preset \"" + std::string(config.preset_of(action)) + "\"";
          issues.push_back(std::move(issue));
        }
        continue;
      }
#endif
      issue.kind = IssueKind::NO_THERMOSTATS;
      issue.text = NO_THERMOSTATS;
      issues.push_back(std::move(issue));
    }
  }
  return issues;
}

ScheduleStatus ScheduleEngine::status(std::string_view id) const {
  ScheduleStatus status;
  const Entry *entry = this->find_(id);
  if (entry == nullptr)
    return status;
  status.issues = this->issues_(entry->config, entry->switches);
  if (!entry->config.enabled)
    return status;
  // Before anything else: nothing runs without the clock, and the editor's order says so first.
  if (!this->clock_valid_) {
    status.state = ScheduleState::WAITING_FOR_CLOCK;
    status.reason = "Waiting for the clock";
    return status;
  }
  if (!entry->active) {
    status.state = ScheduleState::CONFLICT;
    // The one in its way is before it by id; one after it drives a shared target only because
    // this one waits.
    for (const Target &target : entry->config.targets) {
      const Entry *holder = this->holder_(target);
      if (holder != nullptr && holder->config.id < entry->config.id) {
        status.reason = "Schedule \"" + std::string(holder->config.name) + "\" drives " + this->target_name(target);
        break;
      }
    }
    return status;
  }
  status.state = ScheduleState::RUNNING;
#ifdef USE_CLIMATE_HUB
  // Switched off, not merely waiting for its sensor or a relay: that one runs once they come.
  for (const Target &target : entry->config.targets) {
    const climate_hub::ClimateConfig *thermostat =
        target.type == TargetType::THERMOSTAT && global_climate_hub != nullptr
            ? global_climate_hub->store().get(std::string(target.id))
            : nullptr;
    if (thermostat != nullptr && !thermostat->enabled) {
      status.note = "Thermostat \"" + thermostat->name + "\" is stopped; it starts in the scheduled values";
      break;
    }
  }
#endif
  return status;
}

std::string ScheduleEngine::target_name(const Target &target) const {
  if (target.type == TargetType::RELAY) {
#ifdef USE_SWITCH
    const switch_::Switch *sw = find_switch(std::string(target.id));
    if (sw != nullptr)
      return sw->get_name().c_str();
#endif
    return std::string(target.id);
  }
#ifdef USE_CLIMATE_HUB
  const climate_hub::ClimateConfig *thermostat =
      global_climate_hub != nullptr ? global_climate_hub->store().get(std::string(target.id)) : nullptr;
  if (thermostat != nullptr)
    return thermostat->name;
#endif
  return std::string(target.id);
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
  const Entry *holder = this->holder_(Target{type, id});
  return holder == nullptr ? std::string() : std::string(holder->config.id);
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

bool ScheduleEngine::is_name_taken(std::string_view name, std::string_view exclude_id, std::string *error) const {
  const std::string key = name_key(name);
  for (const auto &entry : this->entries_) {
    if (entry->config.id != exclude_id && name_key(entry->config.name) == key) {
      if (error != nullptr)
        *error = "\"" + std::string(name) + "\" is already used by another schedule";
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

bool ScheduleEngine::keeps_room_(const ScheduleConfig &doc, const ScheduleConfig *replaced) const {
  // `doc` is held already; what it replaces goes once it is stored, back to the internal heap
  // only without PSRAM. PSRAM's reserve is kept at the peak, in save_().
  const size_t freed = replaced != nullptr && !this->psram_().has_value() ? replaced->heap_bytes() : 0;
  const size_t internal = saturating_add(this->heap_().free, freed);
  if (internal >= HEAP_KEEP_BYTES)
    return true;
  ESP_LOGE(TAG, "'%s' not written: it would leave %u bytes free, under %u", doc.id.c_str(),
           static_cast<unsigned>(internal), static_cast<unsigned>(HEAP_KEEP_BYTES));
  return false;
}

bool ScheduleEngine::check_target_free_(const ScheduleConfig &doc, std::string_view self_id, Result *result) const {
  for (const Target &target : doc.targets) {
    const Entry *holder = this->holder_(target);
    if (holder == nullptr || holder->config.id == self_id)
      continue;
    *result = failure(409, "\"" + std::string(holder->config.name) + "\" already drives " + this->target_name(target) +
                               "; switch it off first or take over");
    result->holder = holder->config.id;
    return false;
  }
  return true;
}

Result ScheduleEngine::create(ScheduleConfig draft) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  std::string error;
  draft.trim_names();
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
    return failure(409, "Every id made from \"" + std::string(draft.name) +
                            "\" is taken by a file in the schedule folder; choose another name");
  if (draft.enabled && !this->check_target_free_(draft, draft.id, &result))
    return result;
  draft.version = SCHEDULE_VERSION;
  draft.revision = 0;
  if (!this->keeps_room_(draft, nullptr))
    return not_saved(EncodeError::NO_MEMORY);
  EncodeError why = EncodeError::NONE;
  if (!this->save_(draft, &why, true))
    return not_saved(why);
  return this->add_(std::move(draft), "Created");
}

Result ScheduleEngine::update(const std::string &id, ScheduleConfig doc, optional<uint32_t> revision) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  Entry *stored = this->find_(id);
  if (stored == nullptr)
    return failure(404, NOT_FOUND);
  if (stored->config.from_newer_firmware())
    return failure(409, NEWER_FILE);
  if (revision.has_value() && *revision != stored->config.revision)
    return failure(409, STALE_DOCUMENT);
  std::string error;
  doc.trim_names();
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
  doc.revision = stored->config.revision + 1;
  if (!this->keeps_room_(doc, &stored->config))
    return not_saved(EncodeError::NO_MEMORY);
  // Written beside the old file and renamed over it: a failure leaves everything as it was.
  EncodeError why = EncodeError::NONE;
  if (!this->save_(doc, &why, true))
    return not_saved(why);
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
  doc.trim_names();
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
    return failure(
        409, "The id \"" + std::string(doc.id) + "\" is taken by a file in the schedule folder that was not loaded");
  if (doc.enabled && !this->check_target_free_(doc, doc.id, &result))
    return result;
  doc.version = SCHEDULE_VERSION;
  // Never the backup's: a form read before the import would otherwise Save over it.
  doc.revision = stored != nullptr ? stored->config.revision + 1 : 0;
  if (!this->keeps_room_(doc, stored != nullptr ? &stored->config : nullptr))
    return not_saved(EncodeError::NO_MEMORY);
  EncodeError why = EncodeError::NONE;
  if (!this->save_(doc, &why, true))
    return not_saved(why);
  if (stored == nullptr)
    return this->add_(std::move(doc), "Restored");
  return this->replace_(stored, std::move(doc), "Restored");
}

Result ScheduleEngine::add_(ScheduleConfig doc, const char *verb) {
  auto entry = std::make_unique<Entry>();
  entry->config = std::move(doc);
  entry->timer = this->next_timer_++;
  entry->run = this->next_run_++;
  const std::string id(entry->config.id);
  ESP_LOGD(TAG, "%s '%s' (%s)", verb, entry->config.name.c_str(), id.c_str());
  this->resolve_(*entry);
  auto at = std::upper_bound(
      this->entries_.begin(), this->entries_.end(), id,
      [](const std::string &key, const std::unique_ptr<Entry> &e) { return key < std::string_view(e->config.id); });
  this->entries_.insert(at, std::move(entry));
  Result result = success();
  result.id = id;
  this->settle_(id, true, &result);
  return result;
}

Result ScheduleEngine::replace_(Entry *stored, ScheduleConfig doc, const char *verb) {
  // The list's indexes are the old document's.
  this->cancel_(*stored);
  stored->config = std::move(doc);
  this->resolve_(*stored);
  const std::string id(stored->config.id);
  ESP_LOGD(TAG, "%s '%s' (%s)", verb, stored->config.name.c_str(), id.c_str());
  Result result = success();
  result.id = id;
  this->settle_(id, true, &result);
  return result;
}

void ScheduleEngine::settle_(const std::string &id, bool save, Result *result) {
  const std::vector<std::string> started = this->refresh_();
  const Entry *own = this->find_(id);
  if (own != nullptr && own->config.enabled) {
    // The first action the device would skip for good; a held relay only waits.
    for (const Issue &issue : this->issues_(own->config, own->switches)) {
      if (issue.kind == IssueKind::HELD)
        continue;
      result->warning = "skips mode \"" + std::string(own->config.modes[issue.mode].name) + "\", action " +
                        std::to_string(issue.action + 1) + ": " + lower_first(issue.text);
      ESP_LOGW(TAG, "'%s' saved but %s", id.c_str(), result->warning.c_str());
      break;
    }
  }
  const bool own_started = std::find(started.begin(), started.end(), id) != started.end();
  if (own != nullptr && ((save && own->config.enabled) || own_started))
    this->start_(id, save ? "saved" : "enabled");
  for (const std::string &other : started) {
    if (other == id)
      continue;
    ESP_LOGI(TAG, "'%s' drives its targets now", other.c_str());
    this->start_(other, "target free");
    result->started.push_back(other);
  }
}

Result ScheduleEngine::remove(const std::string &id) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  Entry *stored = this->find_(id);
  if (stored == nullptr)
    return failure(404, NOT_FOUND);
  // `id` may be the document's own string, which the removal frees.
  const std::string removed = id;
  this->cancel_(*stored);
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
      stored->config.revision++;
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
      if (other.get() == stored || !other->config.enabled)
        continue;
      const auto &targets = stored->config.targets;
      if (std::none_of(targets.begin(), targets.end(),
                       [&other](const Target &target) { return other->config.names_target(target); }))
        continue;
      other->config.enabled = false;
      other->config.revision++;
      result.persisted = !in_memory && this->save_(other->config) && result.persisted;
      ESP_LOGI(TAG, "'%s' took over from '%s'", id.c_str(), other->config.id.c_str());
      result.stopped.emplace_back(other->config.id);
    }
  }
  if (!stored->config.enabled) {
    stored->config.enabled = true;
    stored->config.revision++;
    result.persisted = this->save_(stored->config) && result.persisted;
  }
  this->settle_(id, false, &result);
  return result;
}

// --- Files ---

std::string ScheduleEngine::folder_() const { return this->storage_->get_base_path() + "/" + this->folder_path_; }

std::string ScheduleEngine::file_path_(std::string_view id) const {
  return this->folder_() + "/" + std::string(id) + ".json";
}

bool ScheduleEngine::ensure_folder_() {
  const std::string path = this->folder_();
  struct stat st;
  if (stat(path.c_str(), &st) == 0)
    return S_ISDIR(st.st_mode);
  return mkdir(path.c_str(), 0755) == 0;
}

// A file the loader refused still holds its id: a new schedule never writes over it.
std::string ScheduleEngine::next_id_(std::string_view name) const {
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

bool ScheduleEngine::save_(const ScheduleConfig &config, EncodeError *why, bool grows) {
  if (why != nullptr)
    *why = EncodeError::NONE;
  if (config.from_newer_firmware()) {
    ESP_LOGW(TAG, "'%s' not written: a newer firmware wrote its file", config.id.c_str());
    return false;
  }
  JsonDocument doc(this->json_allocator_());
  EncodeError encoded = config.encode(doc, this->max_file_bytes_());
  // The peak: the document beside the schedule it came from.
  if (encoded == EncodeError::NONE && grows && !this->has_heap_room())
    encoded = EncodeError::NO_MEMORY;
  if (why != nullptr)
    *why = encoded;
  if (encoded == EncodeError::TOO_LARGE) {
    ESP_LOGE(TAG, "'%s' not written: over %u bytes", config.id.c_str(), static_cast<unsigned>(MAX_FILE_BYTES));
    return false;
  }
  if (encoded == EncodeError::NO_MEMORY) {
    ESP_LOGE(TAG, "'%s' not written: not enough free memory (%s)", config.id.c_str(), this->room_text_().c_str());
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
  FileWriter writer(file);
  serializeJson(doc, writer);
  const bool written = writer.flush();
  const bool closed = fclose(file) == 0;
  if (!written || !closed || rename(tmp.c_str(), path.c_str()) != 0) {
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
