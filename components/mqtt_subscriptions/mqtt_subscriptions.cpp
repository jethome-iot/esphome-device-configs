#include "mqtt_subscriptions.h"
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::mqtt_subscriptions {

static const char *const TAG = "mqtt_subscriptions";
static const char *const FILE_NAME = "subscriptions.json";
static const char *const RENAMED_NOTICE = "unreadable: renamed to subscriptions.json.bad";

MqttSubscriptions *global_mqtt_subscriptions = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

MqttSubscriptions::MqttSubscriptions() { global_mqtt_subscriptions = this; }

void MqttSubscriptions::add_unit(const char *unit, uint8_t uom_index) {
  this->units_.emplace_back(unit);
  this->unit_indices_.push_back(uom_index);
}

void MqttSubscriptions::reserve_sensor_names(const char *prefix, uint8_t count) {
  this->reserved_prefix_ = prefix;
  this->reserved_count_ = count;
}

#ifdef USE_WEBSERVER_SORTING
void MqttSubscriptions::set_web_server_sorting(web_server::WebServer *server, uint64_t group, float weight) {
  this->web_server_ = server;
  this->sorting_group_ = group;
  this->sorting_weight_ = weight;
}
#endif

void MqttSubscriptions::setup() {
  // Asked before mqtt_config's own setup; the first call evaluates the streak for the boot.
  const uint8_t streak = this->config_ != nullptr ? this->config_->crash_streak() : 0;
  this->suspended_ = streak >= mqtt_config::SUSPEND_SUBSCRIPTIONS_AT;
  this->active_.assign(this->max_slots_, SlotConfig{});
  this->running_.assign(this->max_slots_, Running{});

  if (this->storage_ != nullptr && this->storage_->is_mounted()) {
    const SlotFile file = this->read_file_(true);
    if (file.status == SlotFile::Status::NEWER) {
      this->newer_file_ = true;
      ESP_LOGW(TAG, "The subscriptions file is newer firmware's; no slot runs and the file stays as it is");
    } else if (file.status == SlotFile::Status::OK) {
      this->active_ = file.slots;
    }
  } else {
    ESP_LOGW(TAG, "The storage is not mounted; no slot runs");
  }
  this->saved_ = this->active_;

  for (size_t i = 0; i < this->max_slots_; i++)
    this->start_slot_(i);

  mqtt::MQTTClientComponent *client = this->config_ != nullptr ? this->config_->client() : nullptr;
  if (this->suspended_) {
    ESP_LOGW(TAG, "Subscriptions suspended after %u crashes in a row; a restart retries them",
             static_cast<unsigned>(streak));
  } else if (client != nullptr) {
    // One subscription per topic: the client hands a message to each one that matches. It only
    // stores them until it connects, and subscribes again on every connect.
    std::vector<const std::string *> topics;
    for (size_t i = 0; i < this->max_slots_; i++) {
      const std::string &topic = this->active_[i].topic;
      if (this->running_[i].entity == nullptr ||
          std::any_of(topics.begin(), topics.end(), [&topic](const std::string *t) { return *t == topic; }))
        continue;
      topics.push_back(&topic);
      client->subscribe(
          topic, [this](const std::string &t, const std::string &payload) { this->on_message_(t, payload); }, 0);
    }
  }
  this->update_pending_();
  // A restore or an upload rewrites the file with no POST to say so.
  this->set_interval("mqtt-subs-check", this->check_interval_ms_, [this]() { this->check_file_(); });
}

void MqttSubscriptions::start_slot_(size_t index) {
  const SlotConfig &slot = this->active_[index];
  Running &run = this->running_[index];
  if (slot.empty() || !slot.enabled)
    return;
  // The file may be restored or edited by hand, so it is held to the rules a save is. Only
  // slots already running count as taken: of two alike, the first keeps its name.
  std::string error = validate_slot(slot, this->units_);
  if (error.empty()) {
    std::vector<const SlotConfig *> earlier(this->max_slots_, nullptr);
    for (size_t j = 0; j < index; j++) {
      if (this->running_[j].entity != nullptr)
        earlier[j] = &this->active_[j];
    }
    error = this->name_conflict_(index, slot, earlier);
  }
  if (!error.empty()) {
    run.off_reason = "invalid: " + error;
    ESP_LOGW(TAG, "Slot %u '%s' does not run: %s", static_cast<unsigned>(index + 1), slot.name.c_str(), error.c_str());
    return;
  }
  run.entity = this->create_entity_(index, slot);
  if (run.entity == nullptr) {
    run.off_reason = "no room in the entity table";
    ESP_LOGE(TAG, "Slot %u '%s': no room in the entity table", static_cast<unsigned>(index + 1), slot.name.c_str());
  }
}

template<typename L> static bool has_room(const L &list) { return list.size() < list.capacity(); }

EntityBase *MqttSubscriptions::create_entity_(size_t index, const SlotConfig &slot) {
  uint32_t fields = 0;
  EntityBase *entity = nullptr;
  switch (slot.kind) {
    case SlotKind::SENSOR: {
      if (!has_room(App.get_sensors()))
        return nullptr;
      for (size_t i = 0; i < this->units_.size(); i++) {
        if (this->units_[i] == slot.unit)
          fields = static_cast<uint32_t>(this->unit_indices_[i]) << ENTITY_FIELD_UOM_SHIFT;
      }
      auto *sensor = new sensor::Sensor();  // NOLINT(cppcoreguidelines-owning-memory)
      sensor->set_accuracy_decimals(static_cast<int8_t>(slot.decimals));
      sensor->set_state_class(sensor::STATE_CLASS_MEASUREMENT);
      entity = sensor;
      break;
    }
    case SlotKind::BINARY_SENSOR: {
      if (!has_room(App.get_binary_sensors()))
        return nullptr;
      auto *binary = new binary_sensor::BinarySensor();  // NOLINT(cppcoreguidelines-owning-memory)
      // A retained value is the state the topic is in, not an edge: plain callbacks skip it.
      binary->set_trigger_on_initial_state(false);
      entity = binary;
      break;
    }
    case SlotKind::TEXT_SENSOR:
    default: {
      if (!has_room(App.get_text_sensors()))
        return nullptr;
      entity = new text_sensor::TextSensor();  // NOLINT(cppcoreguidelines-owning-memory)
      break;
    }
  }
  // The entity refers to its name rather than copying it, so both live until reboot.
  auto *name = new char[slot.name.size() + 1];  // NOLINT(cppcoreguidelines-owning-memory)
  std::memcpy(name, slot.name.c_str(), slot.name.size() + 1);
  // Hash 0: the object id comes from the name, as codegen's does.
  switch (slot.kind) {
    case SlotKind::SENSOR:
      App.register_sensor(static_cast<sensor::Sensor *>(entity), name, 0, fields);
      break;
    case SlotKind::BINARY_SENSOR:
      App.register_binary_sensor(static_cast<binary_sensor::BinarySensor *>(entity), name, 0, fields);
      break;
    case SlotKind::TEXT_SENSOR:
    default:
      App.register_text_sensor(static_cast<text_sensor::TextSensor *>(entity), name, 0, fields);
      break;
  }
#ifdef USE_WEBSERVER_SORTING
  if (this->web_server_ != nullptr)
    this->web_server_->add_entity_config(entity, this->sorting_weight_ + index, this->sorting_group_);
#endif
  return entity;
}

bool MqttSubscriptions::is_ours_(const EntityBase *entity) const {
  return std::any_of(this->running_.begin(), this->running_.end(),
                     [entity](const Running &run) { return run.entity == entity; });
}

template<typename L>
static const EntityBase *entity_with_id(const L &list, const std::string &id,
                                        const std::function<bool(const EntityBase *)> &skip) {
  char buf[OBJECT_ID_MAX_LEN];
  for (const auto *entity : list) {
    if (entity->is_internal() || skip(entity))
      continue;
    const StringRef taken = entity->get_object_id_to(buf);
    if (taken.size() == id.size() && std::memcmp(taken.c_str(), id.data(), id.size()) == 0)
      return entity;
  }
  return nullptr;
}

std::string MqttSubscriptions::name_conflict_(size_t index, const SlotConfig &slot,
                                              const std::vector<const SlotConfig *> &others) const {
  const std::string id = object_id_of(slot.name);
  for (size_t j = 0; j < others.size(); j++) {
    if (j == index || others[j] == nullptr || others[j]->empty())
      continue;
    if (object_id_of(others[j]->name) == id)
      return "'name' gives the same id as slot " + std::to_string(j + 1) + "; add a Latin letter or a digit";
  }

  // The slots' own entities are covered above, by what is saved rather than what runs.
  const auto skip = [this](const EntityBase *entity) { return this->is_ours_(entity); };
  const EntityBase *taken;
  switch (slot.kind) {
    case SlotKind::SENSOR:
      taken = entity_with_id(App.get_sensors(), id, skip);
      break;
    case SlotKind::BINARY_SENSOR:
      taken = entity_with_id(App.get_binary_sensors(), id, skip);
      break;
    case SlotKind::TEXT_SENSOR:
    default:
      taken = entity_with_id(App.get_text_sensors(), id, skip);
      break;
  }
  if (taken != nullptr)
    return "'name' gives the same id as the entity '" + taken->get_name().str() + "'";

  // Probes are created after this setup and only for a bound slot, so none may exist yet.
  if (slot.kind == SlotKind::SENSOR) {
    for (unsigned n = 1; n <= this->reserved_count_; n++) {
      const std::string probe = this->reserved_prefix_ + " " + std::to_string(n);
      if (object_id_of(probe) == id)
        return "'name' gives the same id as '" + probe + "', which a temperature probe takes";
    }
  }
  return "";
}

void MqttSubscriptions::on_message_(const std::string &topic, const std::string &payload) {
  Message message(payload);
  const uint32_t now = this->now_ms_();
  for (size_t i = 0; i < this->max_slots_; i++) {
    Running &run = this->running_[i];
    const SlotConfig &slot = this->active_[i];
    if (run.entity == nullptr || slot.topic != topic)
      continue;
    run.has_message = true;
    run.last_ms = now;
    run.raw = raw_preview(payload);
    std::string error;
    switch (slot.kind) {
      case SlotKind::SENSOR: {
        NumberReading reading = read_number(message, slot.json_path);
        error = std::move(reading.error);
        // Every message is published, as upstream's mqtt_subscribe sensor does.
        static_cast<sensor::Sensor *>(run.entity)->publish_state(reading.value);
        break;
      }
      case SlotKind::BINARY_SENSOR: {
        BinaryReading reading = read_binary(message, slot.json_path, slot.payload_on, slot.payload_off);
        error = std::move(reading.error);
        auto *binary = static_cast<binary_sensor::BinarySensor *>(run.entity);
        if (reading.value.has_value()) {
          binary->publish_state(*reading.value);
        } else {
          binary->invalidate_state();
        }
        break;
      }
      case SlotKind::TEXT_SENSOR:
      default: {
        TextReading reading = read_text(message, slot.json_path);
        error = std::move(reading.error);
        if (reading.value.has_value())
          static_cast<text_sensor::TextSensor *>(run.entity)->publish_state(*reading.value);
        break;
      }
    }
    this->set_error_(i, error);
  }
}

// Logged when it changes, not on every message of a topic that keeps sending the same thing.
void MqttSubscriptions::set_error_(size_t index, const std::string &error) {
  Running &run = this->running_[index];
  if (run.error == error)
    return;
  const char *name = this->active_[index].name.c_str();
  if (error.empty()) {
    ESP_LOGI(TAG, "Slot %u '%s' reads again", static_cast<unsigned>(index + 1), name);
  } else {
    ESP_LOGW(TAG, "Slot %u '%s': %s", static_cast<unsigned>(index + 1), name, error.c_str());
  }
  run.error = error;
}

// --- The file ---

std::string MqttSubscriptions::folder_path_() const { return this->storage_->get_base_path() + "/" + this->folder_; }

std::string MqttSubscriptions::file_path_() const { return this->folder_path_() + "/" + FILE_NAME; }

MqttSubscriptions::Seen MqttSubscriptions::stat_file_() const {
  Seen seen;
  struct stat st;
  if (stat(this->file_path_().c_str(), &st) == 0) {
    seen.exists = true;
    seen.size = static_cast<int64_t>(st.st_size);
    seen.mtime = st.st_mtime;
  }
  return seen;
}

SlotFile MqttSubscriptions::read_file_(bool rename_bad) {
  const std::string path = this->file_path_();
  this->seen_ = this->stat_file_();
  SlotFile out;
  out.slots.assign(this->max_slots_, SlotConfig{});
  if (!this->seen_.exists)
    return out;  // MISSING
  out.status = SlotFile::Status::UNREADABLE;
  if (this->seen_.size <= static_cast<int64_t>(FILE_MAX)) {
    FILE *file = std::fopen(path.c_str(), "r");
    if (file != nullptr) {
      std::string text(static_cast<size_t>(this->seen_.size), '\0');
      const size_t read = std::fread(&text[0], 1, text.size(), file);
      std::fclose(file);
      out = parse_file(text.data(), read, this->max_slots_);
    }
  }
  if (out.status == SlotFile::Status::UNREADABLE && rename_bad) {
    // Its bytes are kept for whoever wants them; the slots start empty.
    const std::string bad = path + ".bad";
    if (std::rename(path.c_str(), bad.c_str()) == 0) {
      this->boot_notice_ = RENAMED_NOTICE;
      ESP_LOGW(TAG, "%s could not be read; renamed to %s", path.c_str(), bad.c_str());
      this->seen_ = this->stat_file_();
    } else {
      ESP_LOGE(TAG, "%s could not be read, nor renamed", path.c_str());
    }
  }
  return out;
}

bool MqttSubscriptions::write_file_(const std::vector<SlotConfig> &slots) {
  const std::string folder = this->folder_path_();
  struct stat st;
  if (stat(folder.c_str(), &st) != 0 && mkdir(folder.c_str(), 0755) != 0) {
    ESP_LOGE(TAG, "Creating %s failed", folder.c_str());
    return false;
  }
  const std::string text = serialize_file(slots);
  const std::string path = this->file_path_();
  // Written beside the file and renamed over it, so a failed write leaves the old one whole.
  // The close is where a full filesystem shows up.
  const std::string tmp = path + ".tmp";
  FILE *file = std::fopen(tmp.c_str(), "w");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Opening %s failed", tmp.c_str());
    return false;
  }
  const size_t written = std::fwrite(text.data(), 1, text.size(), file);
  const bool closed = std::fclose(file) == 0;
  if (written != text.size() || !closed || std::rename(tmp.c_str(), path.c_str()) != 0) {
    ESP_LOGE(TAG, "Writing %s failed", path.c_str());
    std::remove(tmp.c_str());
    return false;
  }
  this->seen_ = this->stat_file_();
  return true;
}

void MqttSubscriptions::take_saved_(const SlotFile &file) {
  if (file.status == SlotFile::Status::OK) {
    this->saved_ = file.slots;
  } else {
    this->saved_.assign(this->max_slots_, SlotConfig{});  // the next boot would run none
  }
  this->update_pending_();
}

void MqttSubscriptions::check_file_() {
  if (this->storage_ == nullptr || !this->storage_->is_mounted() || this->stat_file_() == this->seen_)
    return;
  ESP_LOGD(TAG, "The subscriptions file changed; reading it again");
  this->take_saved_(this->read_file_(false));
}

// A suspended boot counts too: a restart is what retries the subscriptions.
void MqttSubscriptions::update_pending_() {
  bool any = this->suspended_;
  for (size_t i = 0; i < this->max_slots_; i++)
    any = any || !this->saved_[i].runs_like(this->active_[i]);
  this->pending_any_ = any;
}

const char *MqttSubscriptions::file_error_(SlotFile::Status status) const {
  if (status == SlotFile::Status::NEWER)
    return "newer_firmware";
  if (status == SlotFile::Status::UNREADABLE)
    return "unreadable";
  return this->boot_notice_.empty() ? nullptr : this->boot_notice_.c_str();
}

// --- Device API ---

int MqttSubscriptions::http_status(Result result) {
  switch (result) {
    case Result::OK:
      return 200;
    case Result::INVALID:
      return 400;
    case Result::STORAGE:
    case Result::NEWER_FILE:
    default:
      return 503;
  }
}

void MqttSubscriptions::write_api_json(JsonObject root) {
  const char *file_error = "unavailable";
  if (this->storage_ != nullptr && this->storage_->is_mounted()) {
    const SlotFile file = this->read_file_(false);
    this->take_saved_(file);
    file_error = this->file_error_(file.status);
  }
  root["max_slots"] = this->max_slots_;
  root["reboot_required"] = this->pending_any_.load();
  root["suspended"] = this->suspended_;
  if (file_error == nullptr) {
    root["file_error"] = nullptr;
  } else {
    root["file_error"] = file_error;
  }
  JsonArray units = root["units"].to<JsonArray>();
  for (const std::string &unit : this->units_)
    units.add(unit);

  const uint32_t now = this->now_ms_();
  JsonArray slots = root["slots"].to<JsonArray>();
  for (size_t i = 0; i < this->max_slots_; i++) {
    JsonObject obj = slots.add<JsonObject>();
    obj["slot"] = i + 1;
    this->saved_[i].to_json(obj);
    obj["pending"] = !this->saved_[i].runs_like(this->active_[i]);
    const Running &run = this->running_[i];
    if (run.entity != nullptr) {
      JsonObject entity = obj["entity"].to<JsonObject>();
      entity["domain"] = kind_key(this->active_[i].kind);
      entity["name"] = this->active_[i].name;
    } else {
      obj["entity"] = nullptr;
    }
    JsonObject status = obj["status"].to<JsonObject>();
    status["state"] = state_key(this->state(i));
    status["value"] = this->value_text(i);
    status["raw"] = run.raw;
    const std::string &error = run.entity != nullptr ? run.error : run.off_reason;
    if (error.empty()) {
      status["error"] = nullptr;
    } else {
      status["error"] = error;
    }
    if (run.has_message) {
      status["age_s"] = (now - run.last_ms) / 1000;
    } else {
      status["age_s"] = nullptr;
    }
  }
}

MqttSubscriptions::Result MqttSubscriptions::post(JsonObjectConst body, std::string *message, bool *reboot_required) {
  const auto answer = [&](Result result, std::string text) {
    *message = std::move(text);
    *reboot_required = this->pending_any_.load();
    return result;
  };

  JsonVariantConst number = body["slot"];
  const int64_t slot_number = number.is<int64_t>() ? number.as<int64_t>() : 0;
  if (slot_number < 1 || slot_number > this->max_slots_) {
    return answer(Result::INVALID,
                  "'slot' must be a whole number from 1 to " + std::to_string(static_cast<unsigned>(this->max_slots_)));
  }
  const size_t index = static_cast<size_t>(slot_number - 1);

  JsonVariantConst action = body["action"];
  const bool clear = !action.isUnbound();
  SlotConfig slot;  // a clear saves the empty slot
  if (clear) {
    if (!action.is<const char *>() || action.as<std::string>() != "clear")
      return answer(Result::INVALID, "'action' must be 'clear'");
  } else {
    if (std::string error = read_slot(body, Source::BODY, slot); !error.empty())
      return answer(Result::INVALID, error);
    if (std::string error = validate_slot(slot, this->units_); !error.empty())
      return answer(Result::INVALID, error);
    slot.normalize();
  }

  if (this->storage_ == nullptr || !this->storage_->is_mounted())
    return answer(Result::STORAGE, "Storage unavailable");
  // Read, change one slot, write: a restore or a hand edit since the last look is kept.
  const SlotFile file = this->read_file_(true);
  this->take_saved_(file);
  if (file.status == SlotFile::Status::NEWER)
    return answer(Result::NEWER_FILE, "The subscriptions file was written by newer firmware; it is left as it is");

  std::vector<SlotConfig> next = this->saved_;
  if (!clear) {
    std::vector<const SlotConfig *> others;
    others.reserve(next.size());
    for (const SlotConfig &other : next)
      others.push_back(&other);
    if (std::string error = this->name_conflict_(index, slot, others); !error.empty())
      return answer(Result::INVALID, error);
  }
  if (next[index] == slot)
    return answer(Result::OK, "Nothing changed");
  next[index] = slot;
  if (!this->write_file_(next))
    return answer(Result::STORAGE, "Storage unavailable");
  this->boot_notice_.clear();
  this->saved_ = std::move(next);
  this->update_pending_();

  const std::string label = "Slot " + std::to_string(slot_number);
  if (clear)
    return answer(Result::OK, label + (this->active(index) ? " cleared; its entity goes after a reboot" : " cleared"));
  return answer(Result::OK,
                label + (slot.runs_like(this->active_[index]) ? " saved" : " saved; applies after a reboot"));
}

// --- Loop task ---

bool MqttSubscriptions::active(size_t slot) const {
  return slot < this->running_.size() && this->running_[slot].entity != nullptr;
}

const std::string &MqttSubscriptions::name(size_t slot) const {
  static const std::string NONE;
  return this->active(slot) ? this->active_[slot].name : NONE;
}

bool MqttSubscriptions::has_value(size_t slot) const { return this->active(slot) && this->running_[slot].has_message; }

std::string MqttSubscriptions::value_text(size_t slot) const {
  if (!this->active(slot))
    return "";
  EntityBase *entity = this->running_[slot].entity;
  switch (this->active_[slot].kind) {
    case SlotKind::SENSOR: {
      auto *sensor = static_cast<sensor::Sensor *>(entity);
      if (!sensor->has_state() || std::isnan(sensor->state))
        return "";
      char buf[VALUE_ACCURACY_MAX_LEN];
      const size_t len = value_accuracy_with_uom_to_buf(buf, sensor->state, sensor->get_accuracy_decimals(),
                                                        StringRef(this->active_[slot].unit));
      return std::string(buf, len);
    }
    case SlotKind::BINARY_SENSOR: {
      auto *binary = static_cast<binary_sensor::BinarySensor *>(entity);
      if (!binary->has_state())
        return "";
      return binary->state ? "On" : "Off";
    }
    case SlotKind::TEXT_SENSOR:
    default: {
      auto *text = static_cast<text_sensor::TextSensor *>(entity);
      return text->has_state() ? text->state : "";
    }
  }
}

SlotState MqttSubscriptions::state(size_t slot) const {
  if (!this->active(slot))
    return SlotState::OFF;
  if (this->suspended_)
    return SlotState::SUSPENDED;
  const Running &run = this->running_[slot];
  if (this->config_ == nullptr || !this->config_->connected() || !run.has_message)
    return SlotState::WAITING;
  return run.error.empty() ? SlotState::OK : SlotState::ERROR;
}

const char *MqttSubscriptions::state_key(SlotState state) {
  switch (state) {
    case SlotState::WAITING:
      return "waiting";
    case SlotState::OK:
      return "ok";
    case SlotState::ERROR:
      return "error";
    case SlotState::SUSPENDED:
      return "suspended";
    case SlotState::OFF:
    default:
      return "off";
  }
}

void MqttSubscriptions::dump_config() {
  size_t running = 0;
  for (const Running &run : this->running_)
    running += run.entity != nullptr ? 1 : 0;
  ESP_LOGCONFIG(TAG,
                "MQTT subscriptions:\n"
                "  Slots running: %u of %u\n"
                "  File: %s\n"
                "  Suspended: %s",
                static_cast<unsigned>(running), static_cast<unsigned>(this->max_slots_),
                this->storage_ != nullptr ? this->file_path_().c_str() : "(no storage)", YESNO(this->suspended_));
  if (this->newer_file_)
    ESP_LOGCONFIG(TAG, "  The file is newer firmware's; nothing runs");
  for (size_t i = 0; i < this->max_slots_; i++) {
    const SlotConfig &slot = this->active_[i];
    if (this->running_[i].entity != nullptr) {
      ESP_LOGCONFIG(TAG, "  Slot %u: '%s' (%s) on '%s'", static_cast<unsigned>(i + 1), slot.name.c_str(),
                    kind_key(slot.kind), slot.topic.c_str());
    } else if (!this->running_[i].off_reason.empty()) {
      ESP_LOGCONFIG(TAG, "  Slot %u: off, %s", static_cast<unsigned>(i + 1), this->running_[i].off_reason.c_str());
    }
  }
}

}  // namespace esphome::mqtt_subscriptions
