#include "climate_hub.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>
#include "entity_lookup.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_API
#include "esphome/components/api/api_connection.h"
#include "esphome/components/api/api_server.h"
#endif

namespace esphome {

climate_hub::ClimateHub *global_climate_hub = nullptr;  // NOLINT

namespace climate_hub {

static const char *const TAG = "climate_hub";

// A dragged setpoint slider costs one flash write instead of one per step.
static const uint32_t FLUSH_DEBOUNCE_MS = 3000;
// The renaming search at boot; far more than a pool can collide on.
static const unsigned MAX_NAME_SUFFIX = 99;
// The id search at create; files left in the folder count, so it can run out.
static const unsigned MAX_ID_SUFFIX = 999;

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

static const char *const NOT_FOUND = "Thermostat not found";
static const char *const PRESET_NOT_FOUND = "Preset not found";
// Its fields this firmware does not know would be lost.
static const char *const NEWER_FILE = "A newer firmware wrote this thermostat; update the firmware to change it";
// A form read before the device rewrote the thermostat, after a calibration, say.
static const char *const STALE_DOCUMENT = "The device changed this thermostat since it was read; reload it";
// The dashboard's editor opens a blank form at /climate/new.
static const char *const RESERVED_ID = "new";

// A create or update whose file was not written: nothing changed.
static Result not_saved(bool too_large) {
  if (too_large)
    return failure(413, "The thermostat's file would be over 8 KiB");
  Result result = failure(500, "The thermostat's file could not be written");
  result.persisted = false;
  return result;
}

static Result success() {
  Result result;
  result.ok = true;
  return result;
}

#ifdef USE_SENSOR
// "<who> reports s, not °C": why a sensor that is there cannot feed a thermostat.
static std::string not_celsius(const std::string &who, const sensor::Sensor &sensor) {
  const StringRef unit = sensor.get_unit_of_measurement_ref();
  return who + " reports " + (unit.empty() ? std::string("no unit") : std::string(unit.c_str(), unit.size())) +
         ", not °C";
}
#endif

// For the editor: why `sensor`, found on the device, cannot feed a thermostat; "" when it can.
static std::string unit_refusal(const sensor::Sensor *sensor) {
#ifdef USE_SENSOR
  if (sensor != nullptr && !reports_celsius(*sensor))
    return not_celsius("\"" + std::string(sensor->get_name().c_str()) + "\"", *sensor);
#else
  (void) sensor;
#endif
  return "";
}

// The sensor a thermostat runs on, or nullptr with the reason for the log.
static sensor::Sensor *find_input(const std::string &sensor_id, std::string *error) {
  sensor::Sensor *sensor = find_sensor(sensor_id);
  if (sensor == nullptr) {
    *error = "sensor '" + sensor_id + "' not found";
    return nullptr;
  }
#ifdef USE_SENSOR
  if (!reports_celsius(*sensor)) {
    *error = not_celsius("sensor '" + sensor_id + "'", *sensor);
    return nullptr;
  }
#endif
  return sensor;
}

// What Home Assistant lists of a thermostat's presets: the built-in ones as a set, the custom
// names in order. It reads them only when it lists the entities.
static bool same_preset_listing(const ClimateConfig &a, const ClimateConfig &b) {
  auto listing = [](const ClimateConfig &config) {
    std::pair<uint32_t, std::vector<std::string>> out{0, {}};
    for (const PresetConfig &preset : config.presets) {
      climate::ClimatePreset standard;
      if (standard_preset(preset.name, &standard)) {
        out.first |= 1u << standard;
      } else {
        out.second.push_back(preset.name);
      }
    }
    return out;
  };
  return listing(a) == listing(b);
}

// A Save's presets against the stored ones: a key the thermostat gave out stays with its
// preset, any other is made again from the name; the active preset is the thermostat's state,
// not the form's, and takes new values at once.
static void keep_preset_state(const ClimateConfig &stored, ClimateConfig *doc) {
  for (PresetConfig &preset : doc->presets) {
    if (stored.find_preset(preset.key) == nullptr)
      preset.key.clear();
  }
  doc->assign_preset_keys();
  const PresetConfig *now = doc->find_preset(stored.active_preset);
  doc->active_preset = now != nullptr ? stored.active_preset : "";
  if (now == nullptr)
    return;
  // Found under the stored active key, which always names one of the stored presets.
  const PresetConfig *was = stored.find_preset(stored.active_preset);
  if (now->setpoint != was->setpoint || now->mode != was->mode)
    doc->pick_preset(*now);
}

ClimateHub::ClimateHub() {
  global_climate_hub = this;
  switch_hold::set_holder(this);
}

uint32_t ClimateHub::now_ms() const { return millis(); }

#ifdef USE_WEBSERVER_SORTING
void ClimateHub::set_web_server_sorting(web_server::WebServer *server, uint64_t group, float weight) {
  this->web_server_ = server;
  this->sorting_group_ = group;
  this->sorting_weight_ = weight;
}
#endif

// --- Setup and the loop ---

void ClimateHub::setup() {
  this->dispatcher_.capture_loop_task();
  if (this->storage_ == nullptr || !this->storage_->is_mounted()) {
    ESP_LOGE(TAG, "Storage not mounted");
    this->mark_failed();
    return;
  }
  this->build_pool_();
  if (!this->load_())
    this->mark_failed();
}

void ClimateHub::build_pool_() {
  if (!this->slots_.empty())
    return;
  // Every slot is registered now, while nothing else iterates the climates: the web server's
  // task walks that list, and after setup it must never grow under it.
  auto &climates = App.get_climates();
  for (uint8_t index = 0; index < this->max_controllers_; index++) {
    if (climates.size() >= climates.capacity()) {
      ESP_LOGE(TAG, "No room in the entity table for thermostat %u", static_cast<unsigned>(index) + 1);
      break;
    }
    // Never freed: App, the API and the web server keep the pointer for the life of the device.
    auto *slot = new Slot(this, index);  // NOLINT(cppcoreguidelines-owning-memory)
    App.register_climate(&slot->entity, FREE_SLOT_NAME, 0, this->entity_fields_ | (1u << ENTITY_FIELD_INTERNAL_SHIFT));
#ifdef USE_WEBSERVER_SORTING
    // The sorting map is read by the web server's task too: filled here, never inserted into later.
    if (this->web_server_ != nullptr)
      this->web_server_->add_entity_config(&slot->entity, this->sorting_weight_ + index, this->sorting_group_);
#endif
    this->slots_.push_back(slot);
    this->free_.push_back(slot);
  }
}

bool ClimateHub::load_() {
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
    if (this->store_.size() >= this->max_controllers_) {
      ESP_LOGW(TAG, "'%s' left alone: at most %u thermostats", name.c_str(),
               static_cast<unsigned>(this->max_controllers_));
      continue;
    }
    // setup() runs before the loop feeds the watchdog.
    App.feed_wdt();
    this->load_file_(folder, name);
  }
  this->store_.sort_by_id();

  for (const auto &config : this->store_.all()) {
    if (!config->enabled || this->is_running(config->id))
      continue;
    std::string error;
    if (!this->start_(config.get(), &error))
      ESP_LOGW(TAG, "'%s' %s", config->id.c_str(), this->note_waiting_(config->id, error).c_str());
  }
  ESP_LOGD(TAG, "Loaded %u thermostats from '%s'", static_cast<unsigned>(this->store_.size()), folder.c_str());
  return true;
}

// A file that is refused stays exactly as it is: the folder is writable by hand and over the
// file API, and the file is the only record of what its author meant.
bool ClimateHub::load_file_(const std::string &folder, const std::string &filename) {
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
  if (size <= 0 || static_cast<size_t>(size) > CONFIG_MAX_BYTES) {
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
  ClimateConfig config;
  std::string error;
  if (!config.deserialize(doc.as<JsonObject>(), true, &error)) {
    ESP_LOGE(TAG, "'%s' refused: %s", path.c_str(), error.c_str());
    return false;
  }
  if (config.id != stem) {
    ESP_LOGE(TAG, "'%s' refused: its id '%s' is not its file name", path.c_str(), config.id.c_str());
    return false;
  }
  if (this->store_.get(config.id) != nullptr) {
    ESP_LOGE(TAG, "'%s' refused: id '%s' is loaded already", path.c_str(), config.id.c_str());
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
  this->store_.add(config);
  return true;
}

// Two documents under one name, or a name a YAML climate has: the later one by id becomes
// "<name> 2" and is written back, so the next boot agrees.
void ClimateHub::resolve_name_(ClimateConfig *config) {
  if (!this->is_name_taken(config->name, config->id, nullptr))
    return;
  const std::string original = config->name;
  for (unsigned n = 2; n <= MAX_NAME_SUFFIX; n++) {
    const std::string tail = " " + std::to_string(n);
    const std::string candidate = trim_name(original.substr(0, NAME_MAX_LENGTH - tail.size())) + tail;
    if (this->is_name_taken(candidate, config->id, nullptr))
      continue;
    config->name = candidate;
    ESP_LOGW(TAG, "'%s': the name '%s' is taken, renamed to '%s'", config->id.c_str(), original.c_str(),
             candidate.c_str());
    if (!this->save_(*config))
      ESP_LOGW(TAG, "'%s': the new name was not written", config->id.c_str());
    return;
  }
  ESP_LOGE(TAG, "'%s': no free name for a second '%s'", config->id.c_str(), original.c_str());
}

void ClimateHub::loop() {
  const uint32_t now = this->now_ms();
  for (Slot *slot : this->slots_) {
    if (slot->runtime.running())
      slot->runtime.tick(now);
  }
  if (!this->dirty_.empty() && now - this->dirty_since_ms_ >= FLUSH_DEBOUNCE_MS)
    this->flush_dirty_();
}

void ClimateHub::on_shutdown() { this->flush_dirty_(); }

void ClimateHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Climate hub:");
  if (this->storage_ != nullptr)
    ESP_LOGCONFIG(TAG, "  Folder: %s", this->folder_().c_str());
  ESP_LOGCONFIG(TAG, "  Thermostats: %u of %u", static_cast<unsigned>(this->store_.size()),
                static_cast<unsigned>(this->max_controllers_));
  for (const auto &config : this->store_.all()) {
    std::string state = "disabled";
    if (this->is_running(config->id)) {
      const AutotuneRun *run = this->autotune(config->id);
      state = run != nullptr && run->running() ? "running, calibrating" : "running";
    } else if (config->enabled) {
      state = this->waiting_reason(config->id);
    }
    ESP_LOGCONFIG(TAG, "  '%s' (%s): %s, %s%s", config->name.c_str(), config->id.c_str(),
                  enums::control_kind_to_string(config->kind), state.c_str(),
                  config->from_newer_firmware() ? ", file from a newer firmware" : "");
  }
}

// --- Reads ---

bool ClimateHub::run_on_loop(std::function<bool()> &&job) {
  // A failed component is not scheduled, so a deferred job would only wait out the timeout.
  // Its mutators refuse for that same reason, so nothing moves under a job that runs in place.
  if (this->is_failed())
    return job();
  return this->dispatcher_.run_on_loop(this, std::move(job));
}

ClimateHub::Slot *ClimateHub::slot_for_(const std::string &id) const {
  for (Slot *slot : this->slots_) {
    if (slot->runtime.running() && slot->runtime.config()->id == id)
      return slot;
  }
  return nullptr;
}

const ControllerRuntime *ClimateHub::runtime(const std::string &id) const {
  Slot *slot = this->slot_for_(id);
  return slot == nullptr ? nullptr : &slot->runtime;
}

std::string ClimateHub::waiting_reason(const std::string &id) const {
  const ClimateConfig *config = this->store_.get(id);
  if (config == nullptr || !config->enabled || this->is_running(id))
    return "";
  auto it = this->waiting_.find(id);
  return it == this->waiting_.end() ? std::string() : it->second;
}

std::string ClimateHub::claimed_by(const std::string &relay_object_id) const {
  auto it = this->claims_.find(relay_object_id);
  return it == this->claims_.end() ? std::string() : it->second->owner();
}

std::string ClimateHub::holder_of(const switch_::Switch *sw) const {
  for (const auto &entry : this->claims_) {
    if (entry.second->relay() != sw)
      continue;
    const ClimateConfig *config = this->store_.get(entry.second->owner());
    return config != nullptr ? config->name : entry.second->owner();
  }
  return "";
}

float ClimateHub::sensor_reading(const std::string &sensor_object_id) const {
#ifdef USE_SENSOR
  sensor::Sensor *sensor = find_sensor(sensor_object_id);
  // Shown as a room temperature, so only a number in °C.
  if (sensor != nullptr && sensor->has_state() && reports_celsius(*sensor) && std::isfinite(sensor->state))
    return sensor->state;
#endif
  return NAN;
}

bool ClimateHub::is_name_taken(const std::string &name, const std::string &exclude_id, std::string *error) const {
  // Every name another climate answers to: the other thermostats, then the YAML climates,
  // internal ones too, since the web server matches a name whatever the flag.
  std::vector<std::string> others;
  for (const auto &config : this->store_.all()) {
    if (config->id != exclude_id)
      others.push_back(config->name);
  }
#ifdef USE_CLIMATE
  for (auto *climate : App.get_climates()) {
    // A running thermostat's own entity is in the list above already, under its document.
    const bool ours = std::any_of(this->slots_.begin(), this->slots_.end(),
                                  [climate](const Slot *slot) { return &slot->entity == climate; });
    if (!ours)
      others.emplace_back(climate->get_name().c_str());
  }
#endif
  const std::string key = name_key(name);
  for (const std::string &other : others) {
    if (name_key(other) == key) {
      if (error != nullptr)
        *error = "\"" + name + "\" is already used by another thermostat";
      return true;
    }
  }
  // Home Assistant keys an entity on this id, and drops the second of two that share it.
  const std::string object_id = object_id_of_name(name);
  for (const std::string &other : others) {
    if (object_id_of_name(other) == object_id) {
      if (error != nullptr)
        *error = "\"" + name + "\" is too close to \"" + other + "\": both are " + object_id + " to Home Assistant";
      return true;
    }
  }
  return false;
}

// --- Mutators ---

bool ClimateHub::refuse_if_failed_(Result *result) const {
  if (!this->is_failed())
    return false;
  *result = failure(500, "Thermostat storage is not available");
  return true;
}

Result ClimateHub::create(ClimateConfig draft) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  std::string error;
  draft.name = trim_name(draft.name);
  // The keys are the hub's to give, as the id is.
  for (PresetConfig &preset : draft.presets) {
    preset.name = trim_name(preset.name);
    preset.key.clear();
  }
  draft.active_preset.clear();
  draft.clamp_numbers();
  if (!draft.validate(&error))
    return failure(400, error);
  draft.clamp_setpoint();
  draft.assign_preset_keys();
  if (this->store_.size() >= this->max_controllers_)
    return failure(507, "This device allows " + std::to_string(this->max_controllers_) +
                            " thermostats; delete one to add another");
  if (this->is_name_taken(draft.name, "", &error))
    return failure(409, error);
  draft.id = this->next_id_(draft.name);
  if (draft.id.empty())
    return failure(409, "Every id made from \"" + draft.name +
                            "\" is taken by a file in the thermostat folder; choose another name");
  if (draft.enabled && !this->check_savable_(draft, &result))
    return result;
  draft.version = CONFIG_VERSION;
  draft.revision = 0;
  bool too_large = false;
  if (!this->save_(draft, &too_large))
    return not_saved(too_large);

  ClimateConfig *stored = this->store_.add(draft);
  this->store_.sort_by_id();
  result = success();
  result.id = stored->id;
  if (stored->enabled) {
    if (this->start_(stored, &error)) {
      this->schedule_ha_resync_();
    } else {
      result.warning = this->note_waiting_(stored->id, error);
      ESP_LOGW(TAG, "'%s' created but %s", stored->id.c_str(), result.warning.c_str());
    }
  }
  ESP_LOGD(TAG, "Created '%s' (%s)", stored->name.c_str(), stored->id.c_str());
  return result;
}

Result ClimateHub::update(const std::string &id, ClimateConfig doc, optional<uint32_t> revision) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  ClimateConfig *stored = this->store_.get(id);
  if (stored == nullptr)
    return failure(404, NOT_FOUND);
  if (stored->from_newer_firmware())
    return failure(409, NEWER_FILE);
  if (revision.has_value() && *revision != stored->revision)
    return failure(409, STALE_DOCUMENT);
  std::string error;
  doc.name = trim_name(doc.name);
  for (PresetConfig &preset : doc.presets)
    preset.name = trim_name(preset.name);
  doc.clamp_numbers();
  if (!doc.validate(&error))
    return failure(400, error);
  doc.clamp_setpoint();
  if (this->is_name_taken(doc.name, id, &error))
    return failure(409, error);
  // The path wins: a Save never re-keys a thermostat.
  doc.id = id;
  doc.version = CONFIG_VERSION;
  // The device's to move: a Save builds on the revision it was checked against.
  doc.revision = stored->revision;
  keep_preset_state(*stored, &doc);
  if (doc.enabled && !this->check_savable_(doc, &result))
    return result;
  // Written beside the old file and renamed over it: a failure leaves everything as it was.
  bool too_large = false;
  if (!this->save_(doc, &too_large))
    return not_saved(too_large);

  const ClimateConfig previous = *stored;
  Slot *slot = this->slot_for_(id);
  if (slot != nullptr)
    slot->runtime.end_autotune(AutotuneEnd::SAVED, this->now_ms());
  bool structural = false;
  *stored = doc;
  this->dirty_.erase(id);
  result = success();

  if (!stored->enabled)
    this->waiting_.erase(id);
  if (slot != nullptr && !stored->enabled) {
    this->stop_(slot);
    structural = true;
  } else if (slot != nullptr) {
    if (this->restart_(slot, previous.name, &error)) {
      // A new name or new traits are news to Home Assistant; new gains or preset values are not.
      structural = previous.name != stored->name || previous.supports_heat() != stored->supports_heat() ||
                   previous.supports_cool() != stored->supports_cool() ||
                   previous.visual.min_temperature != stored->visual.min_temperature ||
                   previous.visual.max_temperature != stored->visual.max_temperature ||
                   previous.visual.step != stored->visual.step || !same_preset_listing(previous, *stored);
    } else {
      result.warning = this->note_waiting_(id, error);
      structural = true;
    }
  } else if (stored->enabled) {
    if (this->start_(stored, &error)) {
      structural = true;
    } else {
      result.warning = this->note_waiting_(id, error);
    }
  }
  if (structural)
    this->schedule_ha_resync_();
  if (!result.warning.empty())
    ESP_LOGW(TAG, "'%s' saved but %s", id.c_str(), result.warning.c_str());
  ESP_LOGD(TAG, "Updated '%s' (%s)", stored->name.c_str(), id.c_str());
  // Its own start failed just now, for the reason the warning gives.
  this->start_waiters_(id, &result);
  this->announce_released_();
  return result;
}

Result ClimateHub::remove(const std::string &id) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  if (this->store_.get(id) == nullptr)
    return failure(404, NOT_FOUND);
  Slot *slot = this->slot_for_(id);
  if (slot != nullptr) {
    this->stop_(slot);
    this->schedule_ha_resync_();
  }
  result = success();
  result.persisted = this->delete_file_(id);
  this->dirty_.erase(id);
  this->waiting_.erase(id);
  this->autotunes_.erase(id);
  ESP_LOGD(TAG, "Removed '%s'", id.c_str());
  // Last: `id` may be the document's own string.
  this->store_.remove(id);
  this->start_waiters_("", &result);
  this->announce_released_();
  return result;
}

Result ClimateHub::set_enabled(const std::string &id, bool enabled, bool take_over) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  ClimateConfig *stored = this->store_.get(id);
  if (stored == nullptr)
    return failure(404, NOT_FOUND);
  Slot *slot = this->slot_for_(id);
  result = success();

  if (!enabled) {
    this->waiting_.erase(id);
    if (stored->enabled) {
      stored->enabled = false;
      result.persisted = this->save_(*stored);
      this->dirty_.erase(id);
    }
    if (slot != nullptr) {
      this->stop_(slot);
      this->schedule_ha_resync_();
    }
    this->start_waiters_(id, &result);
    this->announce_released_();
    return result;
  }

  if (slot != nullptr)
    return result;
  // As a Save, but for a relay another thermostat holds or waits for the caller may take over:
  // a missing sensor or relay is waited for.
  Result refused;
  if (!this->check_savable_(*stored, &refused) && (refused.holder.empty() || !take_over))
    return refused;
  std::string error;
  // A take-over stops the others, so only for a thermostat that runs in their place.
  if (!refused.holder.empty() && !this->check_entities_(*stored, &error))
    return failure(400, error);
  // Waiters free no climate entity, a running holder does.
  if (!refused.holder.empty() && this->free_.empty() && this->holder_of_(*stored).empty())
    return failure(409, "No free climate entity to run it in");
  // A newer firmware's file cannot record the take-over, so the others' files do not either:
  // the next boot runs what the files say rather than neither thermostat.
  const bool in_memory = stored->from_newer_firmware();
  auto disable = [&](ClimateConfig *other) {
    if (in_memory) {
      // A change it had waiting is written now, under the flag its file has.
      if (this->dirty_.erase(other->id) != 0)
        this->save_(*other);
      other->enabled = false;
      result.persisted = false;
    } else {
      other->enabled = false;
      result.persisted = this->save_(*other) && result.persisted;
      this->dirty_.erase(other->id);
    }
  };
  // The waiting ones first: a relay handed over below is this one's, and would hide who else
  // names it.
  std::vector<std::string> waiters;
  for (std::string waiter = this->reserver_of_(*stored); !waiter.empty(); waiter = this->reserver_of_(*stored)) {
    disable(this->store_.get(waiter));
    this->waiting_.erase(waiter);
    ESP_LOGI(TAG, "'%s' took the relay over from '%s', which waited", id.c_str(), waiter.c_str());
    waiters.push_back(std::move(waiter));
  }
  // Taken over in the same job, so the relay is never free for a third party in between.
  for (std::string holder = this->holder_of_(*stored); !holder.empty(); holder = this->holder_of_(*stored)) {
    ClimateConfig *held = this->store_.get(holder);
    if (held != nullptr)
      disable(held);
    this->waiting_.erase(holder);
    Slot *holding = this->slot_for_(holder);
    this->hand_over_(holder, holding, *stored);
    if (holding != nullptr) {
      this->stop_(holding, AutotuneEnd::TAKEN_OVER);
    } else {
      this->release_claims_(holder);
    }
    this->schedule_ha_resync_();
    ESP_LOGI(TAG, "'%s' took the relay over from '%s'", id.c_str(), holder.c_str());
    result.stopped.push_back(std::move(holder));
  }
  result.stopped.insert(result.stopped.end(), waiters.begin(), waiters.end());
  if (!stored->enabled) {
    stored->enabled = true;
    result.persisted = this->save_(*stored) && result.persisted;
    this->dirty_.erase(id);
  }
  if (this->start_(stored, &error)) {
    this->schedule_ha_resync_();
  } else {
    result.warning = this->note_waiting_(id, error);
    ESP_LOGW(TAG, "'%s' enabled but %s", id.c_str(), result.warning.c_str());
  }
  // After it: the relays the holders drove alone are free for the next in line.
  this->start_waiters_(id, &result);
  this->announce_released_();
  return result;
}

Result ClimateHub::set_setpoint(const std::string &id, float value) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  ClimateConfig *stored = this->store_.get(id);
  if (stored == nullptr)
    return failure(404, NOT_FOUND);
  if (std::isnan(value))
    return failure(400, "value must be a number");
  const float target = stored->clamp_target(value);
  Slot *slot = this->slot_for_(id);
  if (slot != nullptr) {
    // The path Home Assistant takes: it moves the band, republishes and marks the file dirty.
    auto call = slot->entity.make_call();
    call.set_target_temperature(target);
    call.perform();
  } else if (target != stored->setpoint) {
    stored->setpoint = target;
    this->mark_dirty_(id);
  }
  return success();
}

Result ClimateHub::apply_preset(const std::string &id, const std::string &key) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  ClimateConfig *stored = this->store_.get(id);
  if (stored == nullptr)
    return failure(404, NOT_FOUND);
  const PresetConfig *preset = stored->find_preset(key);
  if (preset == nullptr)
    return failure(404, PRESET_NOT_FOUND);
  Slot *slot = this->slot_for_(id);
  result = success();
  // Running, through the entity, which Home Assistant then hears about.
  if (slot != nullptr ? slot->runtime.pick_preset(*preset, this->now_ms()) : stored->pick_preset(*preset)) {
    this->mark_dirty_(id);
    // A newer firmware's file is never written: the pick lasts until the next boot.
    result.persisted = !stored->from_newer_firmware();
  }
  return result;
}

Result ClimateHub::start_autotune(const std::string &id, optional<AutotuneDirection> direction, AutotuneRule rule) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  const ClimateConfig *stored = this->store_.get(id);
  if (stored == nullptr)
    return failure(404, NOT_FOUND);
  if (stored->kind != ControlKind::PID)
    return failure(409, "Only a PID thermostat can be calibrated");
  Slot *slot = this->slot_for_(id);
  if (slot == nullptr)
    return failure(409, "The thermostat is not running");
  // The gains it finds could not be written back.
  if (stored->from_newer_firmware())
    return failure(409, NEWER_FILE);
  ControllerRuntime &runtime = slot->runtime;
  if (runtime.autotune() != nullptr)
    return failure(409, "A calibration is already running");
  const HubMode mode = stored->mode;
  if (mode == HubMode::OFF)
    return failure(409, "The thermostat is off: set it to heat or cool first");
  if (!direction.has_value()) {
    if (mode == HubMode::HEAT_COOL)
      return failure(400, "In heat_cool, direction must say heat or cool");
    direction = mode == HubMode::COOL ? AutotuneDirection::COOL : AutotuneDirection::HEAT;
  }
  const bool heat = *direction == AutotuneDirection::HEAT;
  const HubMode own = heat ? HubMode::HEAT : HubMode::COOL;
  if (mode != own && mode != HubMode::HEAT_COOL) {
    const std::string word = enums::autotune_direction_to_string(*direction);
    return failure(400, "direction '" + word + "' needs mode " + word + " or heat_cool");
  }
  // It would end at the next pass, before a single swing.
  if (runtime.fault() != HubFault::NONE)
    return failure(409, std::string("The thermostat reports ") + enums::fault_to_string(runtime.fault()) +
                            "; calibrate it once that clears");

  const uint32_t now = this->now_ms();
  auto run = std::make_unique<AutotuneRun>(*direction, rule, PidGains{stored->pid.kp, stored->pid.ki, stored->pid.kd},
                                           stored->setpoint, now);
  AutotuneRun *running = run.get();
  // The last one's numbers go: they stay until the next run.
  this->autotunes_[id] = std::move(run);
  runtime.begin_autotune(running, now);
  ESP_LOGI(TAG, "'%s': calibration started, %s, rule %s", id.c_str(), enums::autotune_direction_to_string(*direction),
           enums::autotune_rule_to_string(rule));
  return success();
}

Result ClimateHub::cancel_autotune(const std::string &id) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  if (this->store_.get(id) == nullptr)
    return failure(404, NOT_FOUND);
  Slot *slot = this->slot_for_(id);
  if (slot == nullptr || slot->runtime.autotune() == nullptr)
    return failure(409, "No calibration is running");
  slot->runtime.end_autotune(AutotuneEnd::CANCELLED, this->now_ms());
  return success();
}

const AutotuneRun *ClimateHub::autotune(const std::string &id) const {
  auto it = this->autotunes_.find(id);
  return it == this->autotunes_.end() ? nullptr : it->second.get();
}

void ClimateHub::complete_autotune_(Slot *slot) {
  ControllerRuntime &runtime = slot->runtime;
  AutotuneRun *run = runtime.autotune();
  ClimateConfig *config = runtime.config();
  bool clamped = false;
  PidGains gains = run->result(&clamped);
  // What runs now is what the file gives back after a reboot.
  gains.kp = as_stored(gains.kp);
  gains.ki = as_stored(gains.ki);
  gains.kd = as_stored(gains.kd);
  config->pid.kp = gains.kp;
  config->pid.ki = gains.ki;
  config->pid.kd = gains.kd;
  // A form read before now would write the old gains back.
  config->revision++;
  const bool persisted = this->save_(*config);
  if (persisted)
    this->dirty_.erase(config->id);
  const uint32_t now = this->now_ms();
  run->succeed(gains, clamped, persisted, now);
  runtime.end_autotune(AutotuneEnd::NONE, now);
  ESP_LOGI(TAG, "'%s': calibrated in %" PRIu32 " s: Ku %.5g, Pu %.0f s, rule %s: kp %.5g, ki %.5g, kd %.5g%s%s%s%s",
           config->id.c_str(), run->elapsed_ms(now) / 1000, run->tuner().ku(), run->tuner().pu(),
           enums::autotune_rule_to_string(run->rule()), gains.kp, gains.ki, gains.kd,
           run->asymmetric() ? ", asymmetric" : "", run->uneven() ? ", uneven" : "", clamped ? ", clamped" : "",
           persisted ? "" : ", not written");
}

// --- Running and stopping ---

std::string ClimateHub::holder_of_(const ClimateConfig &config, std::string *relay_id) const {
  for (const OutputConfig *out : {&config.heat, &config.cool}) {
    if (!out->configured())
      continue;
    const std::string owner = this->claimed_by(out->relay_id);
    if (!owner.empty() && owner != config.id) {
      if (relay_id != nullptr)
        *relay_id = out->relay_id;
      return owner;
    }
  }
  return "";
}

// A running thermostat holds every relay it names, so the others that name one are the waiting.
std::string ClimateHub::reserver_of_(const ClimateConfig &config, std::string *relay_id) const {
  for (const auto &other : this->store_.all()) {
    if (other->id == config.id || !other->enabled || this->is_running(other->id))
      continue;
    for (const OutputConfig *out : {&config.heat, &config.cool}) {
      // One it holds is its own across a Save: whoever waits for it waits on.
      if (!out->configured() || this->claimed_by(out->relay_id) == config.id)
        continue;
      if (other->heat.relay_id == out->relay_id || other->cool.relay_id == out->relay_id) {
        if (relay_id != nullptr)
          *relay_id = out->relay_id;
        return other->id;
      }
    }
  }
  return "";
}

bool ClimateHub::check_entities_(const ClimateConfig &config, std::string *error) const {
  sensor::Sensor *sensor = find_sensor(config.sensor_id);
  if (sensor == nullptr) {
    *error = "No sensor \"" + config.sensor_id + "\" on this device";
    return false;
  }
  *error = unit_refusal(sensor);
  if (!error->empty())
    return false;
  for (const OutputConfig *out : {&config.heat, &config.cool}) {
    if (out->configured() && find_switch(out->relay_id) == nullptr) {
      *error = "No switch \"" + out->relay_id + "\" on this device";
      return false;
    }
  }
  return true;
}

// A missing sensor or relay is no refusal: the thermostat waits for it, as one loaded at boot
// does. A unit never changes, and a held relay is the holder's to give up. A relay an enabled
// thermostat waits for is reserved for it the same way, so no two enabled ones share a relay.
bool ClimateHub::check_savable_(const ClimateConfig &config, Result *result) const {
  const std::string unit = unit_refusal(find_sensor(config.sensor_id));
  if (!unit.empty()) {
    *result = failure(400, unit);
    return false;
  }
  std::string relay_id;
  std::string holder = this->holder_of_(config, &relay_id);
  if (!holder.empty()) {
    *result = this->relay_held_(relay_id, holder, false);
    return false;
  }
  holder = this->reserver_of_(config, &relay_id);
  if (holder.empty())
    return true;
  *result = this->relay_held_(relay_id, holder, true);
  return false;
}

// Both by the names a person knows them by; the holder's id rides along for a take-over.
Result ClimateHub::relay_held_(const std::string &relay_id, const std::string &holder, bool waits) const {
  std::string relay_name = relay_id;
#ifdef USE_SWITCH
  if (switch_::Switch *sw = find_switch(relay_id))
    relay_name = sw->get_name().c_str();
#endif
  const ClimateConfig *held_by = this->store_.get(holder);
  const std::string who = "\"" + (held_by != nullptr ? held_by->name : holder) + "\"";
  Result result = failure(
      409, "\"" + relay_name + "\" is " +
               (waits ? "reserved by " + who + ", which is enabled and waits to start" : "already driven by " + who));
  result.holder = holder;
  return result;
}

ClimateHub::Slot *ClimateHub::take_free_slot_(const std::string &name) {
  const std::string object_id = object_id_of_name(name);
  auto it = std::find_if(this->free_.begin(), this->free_.end(), [&object_id](const Slot *slot) {
    return slot->entity.is_named() && object_id_of(slot->entity) == object_id;
  });
  if (it == this->free_.end())
    it = this->free_.begin();
  Slot *slot = *it;
  this->free_.erase(it);
  return slot;
}

void ClimateHub::park_names_like_(const std::string &name, const Slot *keep) {
  const std::string key = name_key(name);
  const std::string object_id = object_id_of_name(name);
  for (Slot *slot : this->slots_) {
    if (slot == keep || !slot->entity.is_free() || !slot->entity.is_named())
      continue;
    const std::string other = slot->entity.get_name().c_str();
    if (name_key(other) == key || object_id_of_name(other) == object_id)
      slot->entity.park(this->entity_fields_);
  }
}

bool ClimateHub::acquire_claims_(const ClimateConfig &config, RelayClaim **heat, RelayClaim **cool,
                                 std::string *error) {
  *heat = nullptr;
  *cool = nullptr;
  // All checked before one is claimed: a claim let go opens its relay, which something else may
  // have closed.
  for (const OutputConfig *out : {&config.heat, &config.cool}) {
    if (!out->configured())
      continue;
    auto it = this->claims_.find(out->relay_id);
    if (it != this->claims_.end() && it->second->owner() != config.id) {
      *error = "relay '" + out->relay_id + "' is held by '" + it->second->owner() + "'";
      return false;
    }
    if (it == this->claims_.end() && find_switch(out->relay_id) == nullptr) {
      *error = "relay '" + out->relay_id + "' not found";
      return false;
    }
  }
  for (const OutputConfig *out : {&config.heat, &config.cool}) {
    if (!out->configured())
      continue;
    // One it holds already is the same relay across a Save: its state and dwell carry on.
    std::unique_ptr<RelayClaim> &claim = this->claims_[out->relay_id];
    if (claim == nullptr) {
      claim = std::make_unique<RelayClaim>(find_switch(out->relay_id), config.id);
      // Unclaimed since boot, the relay counts as opened at boot, as the reset left it: min_off
      // runs from there, so a boot loop does not short-cycle a compressor.
      auto last = this->relay_history_.find(out->relay_id);
      claim->resume(last != this->relay_history_.end() ? last->second : RelaySwitching{false, 0});
    }
    (out == &config.heat ? *heat : *cool) = claim.get();
  }
  return true;
}

void ClimateHub::release_claims_(const std::string &owner) {
  const uint32_t now = this->now_ms();
  for (auto it = this->claims_.begin(); it != this->claims_.end();)
    it = it->second->owner() == owner ? this->let_go_(it, now) : std::next(it);
}

ClimateHub::ClaimMap::iterator ClimateHub::let_go_(ClaimMap::iterator it, uint32_t now_ms) {
  // Unpaced: once let go, nothing would put it back later.
  it->second->force_off(now_ms, false);
  it->second->last_switching(&this->relay_history_[it->first]);
  this->freed_[it->first] = it->second->relay();
  return this->claims_.erase(it);
}

// So waiting means what it says: one whose relay comes free tries again at once, and one that
// still cannot start gets a fresh reason. A waiter holds no claim, so its start frees nothing.
void ClimateHub::start_waiters_(const std::string &skip_id, Result *result) {
  if (this->freed_.empty())
    return;
  for (const auto &config : this->store_.all()) {
    if (!config->enabled || config->id == skip_id || this->is_running(config->id) ||
        (this->freed_.count(config->heat.relay_id) == 0 && this->freed_.count(config->cool.relay_id) == 0))
      continue;
    std::string error;
    if (this->start_(config.get(), &error)) {
      ESP_LOGI(TAG, "'%s' started: a relay it waited for is free", config->id.c_str());
      result->started.push_back(config->id);
      this->schedule_ha_resync_();
    } else {
      ESP_LOGW(TAG, "'%s' %s", config->id.c_str(), this->note_waiting_(config->id, error).c_str());
    }
  }
}

// Only once the waiters had their turn, and only for a relay still free: a thermostat that
// started on it in the same call keeps it, and nothing that hears of the release moves it
// under that one.
void ClimateHub::announce_released_() {
  std::map<std::string, switch_::Switch *> freed;
  freed.swap(this->freed_);
  for (const auto &relay : freed) {
    if (this->claimed_by(relay.first).empty())
      switch_hold::notify_released(relay.second);
  }
}

// A relay both thermostats drive changes hands as it is, so one that both want closed never
// opens in between; what the holder drives alone goes with the holder.
void ClimateHub::hand_over_(const std::string &from, Slot *holding, const ClimateConfig &to) {
  for (const OutputConfig *out : {&to.heat, &to.cool}) {
    if (!out->configured())
      continue;
    auto it = this->claims_.find(out->relay_id);
    if (it == this->claims_.end() || it->second->owner() != from)
      continue;
    if (holding != nullptr)
      holding->runtime.release_claim(it->second.get());
    it->second->set_owner(to.id);
  }
}

bool ClimateHub::start_(ClimateConfig *config, std::string *error) {
  sensor::Sensor *sensor = find_input(config->sensor_id, error);
  if (sensor == nullptr)
    return false;
  // Before any claim, so a start that fails opens no relay but one a take-over handed it.
  if (this->free_.empty()) {
    this->release_claims_(config->id);
    *error = "no free climate entity";
    return false;
  }
  RelayClaim *heat = nullptr;
  RelayClaim *cool = nullptr;
  if (!this->acquire_claims_(*config, &heat, &cool, error)) {
    this->release_claims_(config->id);
    return false;
  }
  Slot *slot = this->take_free_slot_(config->name);
  const SensorSubscription *sub = this->subscribe_(sensor);
  slot->runtime.start(config, sensor, heat, cool, this->now_ms(), sub != nullptr ? sub->last : Reading{});
  this->park_names_like_(config->name, slot);
  slot->entity.show(config->name, this->entity_fields_);
  slot->entity.publish_state();
  this->waiting_.erase(config->id);
  ESP_LOGD(TAG, "'%s' running as climate '%s'", config->id.c_str(), config->name.c_str());
  return true;
}

const std::string &ClimateHub::note_waiting_(const std::string &id, const std::string &error) {
  return this->waiting_[id] = "not started: " + error;
}

bool ClimateHub::restart_(Slot *slot, const std::string &previous_name, std::string *error) {
  ClimateConfig *config = slot->runtime.config();
  sensor::Sensor *sensor = find_input(config->sensor_id, error);
  if (sensor == nullptr) {
    this->stop_(slot);
    return false;
  }
  RelayClaim *heat = nullptr;
  RelayClaim *cool = nullptr;
  if (!this->acquire_claims_(*config, &heat, &cool, error)) {
    this->stop_(slot);
    return false;
  }
  // A relay the document no longer names is opened and let go; the ones it keeps carry on.
  const uint32_t now = this->now_ms();
  for (auto it = this->claims_.begin(); it != this->claims_.end();) {
    const RelayClaim *claim = it->second.get();
    it = claim->owner() == config->id && claim != heat && claim != cool ? this->let_go_(it, now) : std::next(it);
  }
  const SensorSubscription *sub = this->subscribe_(sensor);
  slot->runtime.start(config, sensor, heat, cool, now, sub != nullptr ? sub->last : Reading{});
  if (config->name != previous_name) {
    this->park_names_like_(config->name, slot);
    slot->entity.show(config->name, this->entity_fields_);
  }
  slot->entity.publish_state();
  return true;
}

void ClimateHub::stop_(Slot *slot, AutotuneEnd why) {
  if (slot == nullptr || !slot->runtime.running())
    return;
  const std::string id = slot->runtime.config()->id;
  slot->runtime.end_autotune(why, this->now_ms());
  slot->runtime.stop(this->now_ms());
  // Its last word under the old name: stopped. Then it drops out of every listing.
  slot->entity.publish_state();
  slot->entity.hide(this->entity_fields_);
  this->release_claims_(id);
  this->free_.push_back(slot);
  ESP_LOGD(TAG, "'%s' stopped", id.c_str());
}

ClimateHub::SensorSubscription *ClimateHub::subscribe_(sensor::Sensor *sensor) {
#ifdef USE_SENSOR
  for (const auto &sub : this->sensor_subs_) {
    if (sub->sensor == sensor)
      return sub.get();
  }
  auto sub = std::make_unique<SensorSubscription>();
  sub->hub = this;
  sub->sensor = sensor;
  SensorSubscription *raw = sub.get();
  sensor->add_on_state_callback([raw](float value) { raw->hub->on_sample_(raw, value); });
  this->sensor_subs_.push_back(std::move(sub));
  return raw;
#else
  return nullptr;
#endif
}

void ClimateHub::on_sample_(SensorSubscription *sub, float value) {
  // NaN slips past the staleness and over-temperature guards and poisons the integral; an
  // infinity latches the heater or winds the integral. Neither is a reading: the sensor goes stale.
  if (!std::isfinite(value))
    return;
  const uint32_t now = this->now_ms();
  sub->last = Reading{value, now, true};
  for (Slot *slot : this->slots_) {
    if (!slot->runtime.running() || slot->runtime.sensor() != sub->sensor)
      continue;
    slot->runtime.on_sample(value, now);
    const AutotuneRun *run = slot->runtime.autotune();
    if (run != nullptr && run->found())
      this->complete_autotune_(slot);
  }
}

void ClimateHub::on_control_(uint8_t index, const climate::ClimateCall &call) {
  if (index >= this->slots_.size())
    return;
  ControllerRuntime &runtime = this->slots_[index]->runtime;
  if (!runtime.running())
    return;
  if (runtime.control(call, this->now_ms()))
    this->mark_dirty_(runtime.config()->id);
}

// --- Files ---

std::string ClimateHub::folder_() const { return this->storage_->get_base_path() + "/" + this->folder_path_; }

std::string ClimateHub::file_path_(const std::string &id) const { return this->folder_() + "/" + id + ".json"; }

bool ClimateHub::ensure_folder_() {
  const std::string path = this->folder_();
  struct stat st;
  if (stat(path.c_str(), &st) == 0)
    return S_ISDIR(st.st_mode);
  return mkdir(path.c_str(), 0755) == 0;
}

// A file the loader refused still holds its id: a new thermostat never writes over it.
std::string ClimateHub::next_id_(const std::string &name) const {
  const std::string base = slugify_id(name);
  for (unsigned n = 1; n <= MAX_ID_SUFFIX; n++) {
    const std::string candidate = id_with_suffix(base, n);
    if (candidate == RESERVED_ID)
      continue;
    if (this->store_.get(candidate) == nullptr && !file_exists(this->file_path_(candidate)))
      return candidate;
  }
  return "";
}

bool ClimateHub::save_(const ClimateConfig &config, bool *too_large) {
  if (too_large != nullptr)
    *too_large = false;
  if (config.from_newer_firmware()) {
    ESP_LOGW(TAG, "'%s' not written: a newer firmware wrote its file", config.id.c_str());
    return false;
  }
  std::string json;
  const EncodeError encoded = this->encode_(config, &json);
  if (too_large != nullptr)
    *too_large = encoded == EncodeError::TOO_LARGE;
  if (encoded == EncodeError::TOO_LARGE) {
    ESP_LOGE(TAG, "'%s' not written: over %u bytes", config.id.c_str(), static_cast<unsigned>(CONFIG_MAX_BYTES));
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

bool ClimateHub::remove_file_(const std::string &path) { return ::remove(path.c_str()) == 0; }

bool ClimateHub::delete_file_(const std::string &id) {
  const std::string path = this->file_path_(id);
  if (this->remove_file_(path) || !file_exists(path))
    return true;
  // Left whole, the loader would bring the thermostat back at the next boot; blank, it refuses it.
  ESP_LOGW(TAG, "Cannot delete '%s'; blanking it", path.c_str());
  FILE *blank = fopen(path.c_str(), "w");
  if (blank != nullptr && fclose(blank) == 0)
    return true;
  ESP_LOGE(TAG, "Cannot blank '%s': it comes back at the next boot", path.c_str());
  return false;
}

void ClimateHub::mark_dirty_(const std::string &id) {
  // What a newer firmware wrote is not this one's to rewrite: the change lives in memory.
  const ClimateConfig *config = this->store_.get(id);
  if (config != nullptr && config->from_newer_firmware())
    return;
  if (this->dirty_.empty())
    this->dirty_since_ms_ = this->now_ms();
  this->dirty_.insert(id);
}

void ClimateHub::flush_dirty_() {
  for (const std::string &id : this->dirty_) {
    const ClimateConfig *config = this->store_.get(id);
    if (config != nullptr && !this->save_(*config))
      ESP_LOGW(TAG, "'%s': the thermostat's state was not written", id.c_str());
  }
  this->dirty_.clear();
}

// --- Home Assistant ---

void ClimateHub::schedule_ha_resync_() {
  this->set_timeout("ha_resync", this->ha_resync_delay_ms_, [this]() { this->resync_home_assistant_(); });
}

// Upstream's own way of making clients reconnect (it does the same after a new API key). A
// client that cannot take the request is dropped, which Home Assistant answers the same way.
void ClimateHub::resync_home_assistant_() {
#ifdef USE_API
  if (api::global_api_server == nullptr)
    return;
  ESP_LOGI(TAG, "Thermostats changed: asking Home Assistant to reconnect");
  for (const auto &client : api::global_api_server->active_clients()) {
    if (!client->is_connection_setup())
      continue;
    api::DisconnectRequest request;
    if (!client->send_message(request))
      client->on_fatal_error();
  }
#endif
}

}  // namespace climate_hub
}  // namespace esphome
