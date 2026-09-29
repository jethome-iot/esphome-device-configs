#include "climate_hub.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
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
// The dashboard's editor opens a blank form at /climate/new.
static const char *const RESERVED_ID = "new";

static Result success() {
  Result result;
  result.ok = true;
  return result;
}

ClimateHub::ClimateHub() { global_climate_hub = this; }

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
      ESP_LOGW(TAG, "'%s' not started: %s", config->id.c_str(), error.c_str());
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
    ESP_LOGCONFIG(TAG, "  '%s' (%s): %s, %s", config->name.c_str(), config->id.c_str(),
                  enums::control_kind_to_string(config->kind),
                  this->is_running(config->id) ? "running" : (config->enabled ? "not started" : "disabled"));
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

std::string ClimateHub::claimed_by(const std::string &relay_object_id) const {
  auto it = this->claims_.find(relay_object_id);
  return it == this->claims_.end() ? std::string() : it->second->owner();
}

float ClimateHub::sensor_reading(const std::string &sensor_object_id) const {
#ifdef USE_SENSOR
  sensor::Sensor *sensor = find_sensor(sensor_object_id);
  if (sensor != nullptr && sensor->has_state())
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
  draft.clamp_numbers();
  if (!draft.validate(&error))
    return failure(400, error);
  draft.clamp_setpoint();
  if (this->store_.size() >= this->max_controllers_)
    return failure(507, "This device allows " + std::to_string(this->max_controllers_) +
                            " thermostats; delete one to add another");
  if (this->is_name_taken(draft.name, "", &error))
    return failure(409, error);
  draft.id = this->next_id_(draft.name);
  if (draft.id.empty())
    return failure(409, "Every id made from \"" + draft.name +
                            "\" is taken by a file in the thermostat folder; choose another name");
  if (draft.enabled && !this->check_startable_(draft, &result))
    return result;
  draft.version = 1;
  if (!this->save_(draft)) {
    result = failure(500, "The thermostat's file could not be written");
    result.persisted = false;
    return result;
  }

  ClimateConfig *stored = this->store_.add(draft);
  this->store_.sort_by_id();
  result = success();
  result.id = stored->id;
  if (stored->enabled) {
    if (this->start_(stored, &error)) {
      this->schedule_ha_resync_();
    } else {
      result.warning = "not started: " + error;
      ESP_LOGW(TAG, "'%s' created but %s", stored->id.c_str(), result.warning.c_str());
    }
  }
  ESP_LOGD(TAG, "Created '%s' (%s)", stored->name.c_str(), stored->id.c_str());
  return result;
}

Result ClimateHub::update(const std::string &id, ClimateConfig doc) {
  Result result;
  if (this->refuse_if_failed_(&result))
    return result;
  ClimateConfig *stored = this->store_.get(id);
  if (stored == nullptr)
    return failure(404, NOT_FOUND);
  std::string error;
  doc.name = trim_name(doc.name);
  doc.clamp_numbers();
  if (!doc.validate(&error))
    return failure(400, error);
  doc.clamp_setpoint();
  if (this->is_name_taken(doc.name, id, &error))
    return failure(409, error);
  // The path wins: a Save never re-keys a thermostat.
  doc.id = id;
  doc.version = 1;
  if (doc.enabled && !this->check_startable_(doc, &result))
    return result;
  // Written beside the old file and renamed over it: a failure leaves everything as it was.
  if (!this->save_(doc)) {
    result = failure(500, "The thermostat's file could not be written");
    result.persisted = false;
    return result;
  }

  const ClimateConfig previous = *stored;
  Slot *slot = this->slot_for_(id);
  bool structural = false;
  *stored = doc;
  this->dirty_.erase(id);
  result = success();

  if (slot != nullptr && !stored->enabled) {
    this->stop_(slot);
    structural = true;
  } else if (slot != nullptr) {
    if (this->restart_(slot, previous.name, &error)) {
      // A new name or new traits are news to Home Assistant; new gains are not.
      structural = previous.name != stored->name || previous.supports_heat() != stored->supports_heat() ||
                   previous.supports_cool() != stored->supports_cool() ||
                   previous.visual.min_temperature != stored->visual.min_temperature ||
                   previous.visual.max_temperature != stored->visual.max_temperature ||
                   previous.visual.step != stored->visual.step;
    } else {
      result.warning = "not started: " + error;
      structural = true;
    }
  } else if (stored->enabled) {
    if (this->start_(stored, &error)) {
      structural = true;
    } else {
      result.warning = "not started: " + error;
    }
  }
  if (structural)
    this->schedule_ha_resync_();
  if (!result.warning.empty())
    ESP_LOGW(TAG, "'%s' saved but %s", id.c_str(), result.warning.c_str());
  ESP_LOGD(TAG, "Updated '%s' (%s)", stored->name.c_str(), id.c_str());
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
  ESP_LOGD(TAG, "Removed '%s'", id.c_str());
  // Last: `id` may be the document's own string.
  this->store_.remove(id);
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
    if (stored->enabled) {
      stored->enabled = false;
      result.persisted = this->save_(*stored);
      this->dirty_.erase(id);
    }
    if (slot != nullptr) {
      this->stop_(slot);
      this->schedule_ha_resync_();
    }
    return result;
  }

  if (slot != nullptr)
    return result;
  std::string error;
  if (!this->check_entities_(*stored, &error))
    return failure(400, error);
  // Taken over in the same job, so the relay is never free for a third party in between.
  std::string relay_id;
  for (std::string holder = this->holder_of_(*stored, &relay_id); !holder.empty();
       holder = this->holder_of_(*stored, &relay_id)) {
    if (!take_over)
      return this->relay_held_(relay_id, holder);
    ClimateConfig *held = this->store_.get(holder);
    if (held != nullptr) {
      held->enabled = false;
      result.persisted = this->save_(*held) && result.persisted;
      this->dirty_.erase(holder);
    }
    Slot *holding = this->slot_for_(holder);
    this->hand_over_(holder, holding, *stored);
    if (holding != nullptr) {
      this->stop_(holding);
    } else {
      this->release_claims_(holder);
    }
    this->schedule_ha_resync_();
    ESP_LOGI(TAG, "'%s' took the relay over from '%s'", id.c_str(), holder.c_str());
  }
  if (!stored->enabled) {
    stored->enabled = true;
    result.persisted = this->save_(*stored) && result.persisted;
    this->dirty_.erase(id);
  }
  if (this->start_(stored, &error)) {
    this->schedule_ha_resync_();
  } else {
    result.warning = "not started: " + error;
    ESP_LOGW(TAG, "'%s' enabled but %s", id.c_str(), result.warning.c_str());
  }
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
  const float target = std::max(stored->visual.min_temperature, std::min(stored->visual.max_temperature, value));
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

bool ClimateHub::check_entities_(const ClimateConfig &config, std::string *error) const {
  if (find_sensor(config.sensor_id) == nullptr) {
    *error = "No sensor \"" + config.sensor_id + "\" on this device";
    return false;
  }
  for (const OutputConfig *out : {&config.heat, &config.cool}) {
    if (out->configured() && find_switch(out->relay_id) == nullptr) {
      *error = "No switch \"" + out->relay_id + "\" on this device";
      return false;
    }
  }
  return true;
}

bool ClimateHub::check_startable_(const ClimateConfig &config, Result *result) const {
  std::string error;
  if (!this->check_entities_(config, &error)) {
    *result = failure(400, error);
    return false;
  }
  std::string relay_id;
  const std::string holder = this->holder_of_(config, &relay_id);
  if (holder.empty())
    return true;
  *result = this->relay_held_(relay_id, holder);
  return false;
}

// Both by the names a person knows them by; the holder's id rides along for a take-over.
Result ClimateHub::relay_held_(const std::string &relay_id, const std::string &holder) const {
  std::string relay_name = relay_id;
#ifdef USE_SWITCH
  if (switch_::Switch *sw = find_switch(relay_id))
    relay_name = sw->get_name().c_str();
#endif
  const ClimateConfig *held_by = this->store_.get(holder);
  Result result = failure(
      409, "\"" + relay_name + "\" is already driven by \"" + (held_by != nullptr ? held_by->name : holder) + "\"");
  result.holder = holder;
  return result;
}

ClimateHub::Slot *ClimateHub::take_free_slot_(const std::string &name) {
  if (this->free_.empty())
    return nullptr;
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
  for (const OutputConfig *out : {&config.heat, &config.cool}) {
    if (!out->configured())
      continue;
    RelayClaim *claim = nullptr;
    auto it = this->claims_.find(out->relay_id);
    if (it != this->claims_.end()) {
      if (it->second->owner() != config.id) {
        *error = "relay '" + out->relay_id + "' is held by '" + it->second->owner() + "'";
        return false;
      }
      // The same relay across a Save: its state and dwell carry on.
      claim = it->second.get();
    } else {
      switch_::Switch *sw = find_switch(out->relay_id);
      if (sw == nullptr) {
        *error = "relay '" + out->relay_id + "' not found";
        return false;
      }
      auto created = std::make_unique<RelayClaim>(sw, config.id);
      auto last = this->relay_history_.find(out->relay_id);
      if (last != this->relay_history_.end())
        created->resume(last->second);
      claim = created.get();
      this->claims_[out->relay_id] = std::move(created);
    }
    (out == &config.heat ? *heat : *cool) = claim;
  }
  return true;
}

void ClimateHub::release_claims_(const std::string &owner) {
  const uint32_t now = this->now_ms();
  for (auto it = this->claims_.begin(); it != this->claims_.end();)
    it = it->second->owner() == owner ? this->let_go_(it, now) : std::next(it);
}

ClimateHub::ClaimMap::iterator ClimateHub::let_go_(ClaimMap::iterator it, uint32_t now_ms) {
  it->second->force_off(now_ms);
  it->second->last_switching(&this->relay_history_[it->first]);
  return this->claims_.erase(it);
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
  sensor::Sensor *sensor = find_sensor(config->sensor_id);
  if (sensor == nullptr) {
    *error = "sensor '" + config->sensor_id + "' not found";
    return false;
  }
  RelayClaim *heat = nullptr;
  RelayClaim *cool = nullptr;
  if (!this->acquire_claims_(*config, &heat, &cool, error)) {
    this->release_claims_(config->id);
    return false;
  }
  Slot *slot = this->take_free_slot_(config->name);
  if (slot == nullptr) {
    this->release_claims_(config->id);
    *error = "no free climate entity";
    return false;
  }
  const SensorSubscription *sub = this->subscribe_(sensor);
  slot->runtime.start(config, sensor, heat, cool, this->now_ms(), sub != nullptr ? sub->last : Reading{});
  this->park_names_like_(config->name, slot);
  slot->entity.show(config->name, this->entity_fields_);
  slot->entity.publish_state();
  ESP_LOGD(TAG, "'%s' running as climate '%s'", config->id.c_str(), config->name.c_str());
  return true;
}

bool ClimateHub::restart_(Slot *slot, const std::string &previous_name, std::string *error) {
  ClimateConfig *config = slot->runtime.config();
  sensor::Sensor *sensor = find_sensor(config->sensor_id);
  if (sensor == nullptr) {
    *error = "sensor '" + config->sensor_id + "' not found";
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

void ClimateHub::stop_(Slot *slot) {
  if (slot == nullptr || !slot->runtime.running())
    return;
  const std::string id = slot->runtime.config()->id;
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
  // A NaN stored as a reading would pass the staleness and over-temperature guards, and one
  // through the integrator would leave it NaN for good.
  if (std::isnan(value))
    return;
  const uint32_t now = this->now_ms();
  sub->last = Reading{value, now, true};
  for (Slot *slot : this->slots_) {
    if (slot->runtime.running() && slot->runtime.sensor() == sub->sensor)
      slot->runtime.on_sample(value, now);
  }
}

void ClimateHub::on_control_(uint8_t index, const climate::ClimateCall &call) {
  if (index >= this->slots_.size())
    return;
  ControllerRuntime &runtime = this->slots_[index]->runtime;
  if (runtime.running() && runtime.control(call))
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

bool ClimateHub::save_(const ClimateConfig &config) {
  if (!this->ensure_folder_()) {
    ESP_LOGE(TAG, "Cannot create '%s'", this->folder_().c_str());
    return false;
  }
  JsonDocument doc;
  config.serialize(doc.to<JsonObject>());
  std::string json;
  serializeJson(doc, json);

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
  if (this->dirty_.empty())
    this->dirty_since_ms_ = this->now_ms();
  this->dirty_.insert(id);
}

void ClimateHub::flush_dirty_() {
  for (const std::string &id : this->dirty_) {
    const ClimateConfig *config = this->store_.get(id);
    if (config != nullptr && !this->save_(*config))
      ESP_LOGW(TAG, "'%s': the new target or mode was not written", id.c_str());
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
