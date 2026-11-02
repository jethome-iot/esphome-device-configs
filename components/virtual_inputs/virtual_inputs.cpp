#include "virtual_inputs.h"

#include <algorithm>
#include <cstdio>

#include "esphome/core/application.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_API
#include "esphome/components/api/api_connection.h"
#include "esphome/components/api/api_server.h"
#endif

namespace esphome::virtual_inputs {

static const char *const TAG = "virtual_inputs";
static constexpr uint32_t INTERNAL_BIT = 1u << ENTITY_FIELD_INTERNAL_SHIFT;
static const InputConfig FREE_SLOT{};

VirtualInput::VirtualInput(uint8_t index) {
  snprintf(this->name_, sizeof(this->name_), "Virtual input %u", static_cast<unsigned>(index) + 1);
}

void VirtualInput::set_visible(bool visible, uint32_t entity_fields) {
  // Hash 0: derived from the name, as codegen does, so object-id-keyed records find it.
  this->configure_entity_(this->name_, 0, visible ? entity_fields & ~INTERNAL_BIT : entity_fields | INTERNAL_BIT);
}

void VirtualInputs::set_max_inputs(uint8_t count) {
  this->max_inputs_ = count;
  this->file_.set_slots(count);
}

#ifdef USE_WEBSERVER_SORTING
void VirtualInputs::set_web_server_sorting(web_server::WebServer *server, uint64_t group, float weight) {
  this->web_server_ = server;
  this->sorting_group_ = group;
  this->sorting_weight_ = weight;
}
#endif

uint32_t VirtualInputs::now_ms_() const { return millis(); }

// --- Setup and the loop ---

void VirtualInputs::setup() {
  this->build_pool_();
  this->load_();
  const uint32_t now = this->now_ms_();
  for (Slot *slot : this->slots_) {
    if (!slot->config.configured())
      continue;
    slot->entity.set_visible(true, 0);
    this->start_(slot, now);
  }
}

void VirtualInputs::build_pool_() {
  if (!this->slots_.empty())
    return;
  // Every slot is registered now, while nothing else walks the binary sensors: the web server's
  // task and the API do afterwards, and the list must never grow under them.
  auto &sensors = App.get_binary_sensors();
  for (uint8_t index = 0; index < this->max_inputs_; index++) {
    if (sensors.size() >= sensors.capacity()) {
      ESP_LOGE(TAG, "No room in the entity table for virtual input %u", static_cast<unsigned>(index) + 1);
      break;
    }
    // Never freed: App, the API and the web server keep the pointer for the life of the device.
    auto *slot = new Slot(index);  // NOLINT(cppcoreguidelines-owning-memory)
    App.register_binary_sensor(&slot->entity, slot->entity.fixed_name(), 0, INTERNAL_BIT);
#ifdef USE_WEBSERVER_SORTING
    // The sorting map is read by the web server's task too: filled here, never inserted into later.
    if (this->web_server_ != nullptr)
      this->web_server_->add_entity_config(&slot->entity, this->sorting_weight_ + index, this->sorting_group_);
#endif
#ifdef ENTITY_CONFIG_BINARY_SENSOR
    if (this->input_settings_ != nullptr)
      this->input_settings_->set_label_only(&slot->entity);
#endif
    this->slots_.push_back(slot);
  }
}

void VirtualInputs::load_() {
  this->file_loaded_ = true;
  for (Slot *slot : this->slots_)
    slot->config = InputConfig{};
  // The keeper has set up by now: no mount means it failed, and there is no file to read.
  if (!this->can_save()) {
    ESP_LOGW(TAG, "Storage unavailable: no virtual inputs this boot");
    return;
  }
  if (!this->file_.load_from_file(this->keeper_->get_storage(), this->keeper_->get_config_dir())) {
    // Left as it is for a person to fix: a save would write the slots this boot knows over it.
    this->file_loaded_ = false;
    ESP_LOGE(TAG, "%s.json did not load: no virtual inputs until it is fixed and the device rebooted",
             this->file_.get_key());
    return;
  }
  const auto &table = this->file_.table();
  for (size_t index = 0; index < this->slots_.size() && index < table.size(); index++)
    this->slots_[index]->config = table[index];
}

void VirtualInputs::loop() {
  const uint32_t now = this->now_ms_();
  for (Slot *slot : this->slots_) {
    if (slot->config.configured() && slot->line.tick(now))
      this->publish_(slot);
  }
}

void VirtualInputs::dump_config() {
  ESP_LOGCONFIG(TAG, "Virtual inputs: %u of %u slots", static_cast<unsigned>(this->file_.size()),
                static_cast<unsigned>(this->max_inputs_));
  if (this->keeper_ != nullptr)
    ESP_LOGCONFIG(TAG, "  File: %s/%s.json%s", this->keeper_->get_config_dir().c_str(), this->file_.get_key(),
                  this->file_loaded_ ? "" : " (did not load)");
  for (const Slot *slot : this->slots_) {
    if (!slot->config.configured())
      continue;
    ESP_LOGCONFIG(TAG, "  %s: follows '%s', on after %us, off after %us", slot->entity.fixed_name(),
                  slot->config.source.c_str(), static_cast<unsigned>(slot->config.on_delay),
                  static_cast<unsigned>(slot->config.off_delay));
  }
}

// --- The sources ---

binary_sensor::BinarySensor *VirtualInputs::find_source_(const std::string &object_id) const {
  if (object_id.empty())
    return nullptr;
  const uint32_t key = fnv1_hash(object_id);
  for (auto *sensor : App.get_binary_sensors()) {
    if (sensor->get_object_id_hash() == key && !sensor->is_internal() && !this->is_virtual_input(sensor))
      return sensor;
  }
  return nullptr;
}

SourceCheck VirtualInputs::check_source(const std::string &object_id) const {
  // By the slot's own name, visible or not: one virtual input never follows another.
  const uint32_t key = fnv1_hash(object_id);
  for (const Slot *slot : this->slots_) {
    if (slot->entity.get_object_id_hash() == key)
      return SourceCheck::VIRTUAL;
  }
  return this->find_source_(object_id) != nullptr ? SourceCheck::OK : SourceCheck::MISSING;
}

std::vector<binary_sensor::BinarySensor *> VirtualInputs::sources() const {
  std::vector<binary_sensor::BinarySensor *> found;
  for (auto *sensor : App.get_binary_sensors()) {
    if (!sensor->is_internal() && !this->is_virtual_input(sensor))
      found.push_back(sensor);
  }
  return found;
}

void VirtualInputs::subscribe_(binary_sensor::BinarySensor *source) {
  if (std::find(this->subscribed_.begin(), this->subscribed_.end(), source) != this->subscribed_.end())
    return;
  this->subscribed_.push_back(source);
  // Full-state: the first level after boot has no previous one, and it is the one a slot takes.
  source->add_full_state_callback([this, source](optional<bool> /*previous*/, optional<bool> current) {
    if (current.has_value())
      this->on_source_(source, *current);
  });
}

void VirtualInputs::on_source_(binary_sensor::BinarySensor *source, bool level) {
  const uint32_t now = this->now_ms_();
  for (Slot *slot : this->slots_) {
    if (!slot->config.configured() || slot->source != source)
      continue;
    slot->line.feed(level, now);
    this->publish_(slot);
  }
}

void VirtualInputs::start_(Slot *slot, uint32_t now) {
  slot->line.set_delays(slot->config.on_delay * 1000, slot->config.off_delay * 1000);
  slot->source = this->find_source_(slot->config.source);
  if (slot->source == nullptr) {
    // Off for good until a save names a source this firmware has; problem() says why.
    ESP_LOGW(TAG, "%s: source '%s' is not an input here, staying off", slot->entity.fixed_name(),
             slot->config.source.c_str());
    slot->line.reset();
    this->publish_(slot);
    return;
  }
  this->subscribe_(slot->source);
  // A source not up yet hands over its first level through the callback.
  if (!slot->source->has_state()) {
    slot->line.forget_level();
    return;
  }
  slot->line.restart(slot->source->state, now);
  this->publish_(slot);
}

void VirtualInputs::publish_(Slot *slot) {
  if (!slot->entity.has_state() || slot->entity.state != slot->line.output())
    slot->entity.publish_state(slot->line.output());
}

// --- What the API and the panel read ---

bool VirtualInputs::configured(size_t slot) const {
  return slot < this->slots_.size() && this->slots_[slot]->config.configured();
}

const InputConfig &VirtualInputs::config(size_t slot) const {
  return slot < this->slots_.size() ? this->slots_[slot]->config : FREE_SLOT;
}

binary_sensor::BinarySensor *VirtualInputs::sensor(size_t slot) const {
  return slot < this->slots_.size() ? &this->slots_[slot]->entity : nullptr;
}

bool VirtualInputs::is_virtual_input(const binary_sensor::BinarySensor *sensor) const {
  return std::any_of(this->slots_.begin(), this->slots_.end(),
                     [sensor](const Slot *slot) { return &slot->entity == sensor; });
}

std::string VirtualInputs::problem(size_t slot) const {
  if (!this->configured(slot))
    return "";
  const Slot *entry = this->slots_[slot];
  const char *source = entry->config.source.c_str();
  switch (this->check_source(entry->config.source)) {
    case SourceCheck::MISSING:
      return str_sprintf("Source '%s' not found", source);
    case SourceCheck::VIRTUAL:
      return str_sprintf("Source '%s' is a virtual input", source);
    case SourceCheck::OK:
      break;
  }
  if (entry->source == nullptr || !entry->line.has_level())
    return str_sprintf("Waiting for '%s' to report", source);
  return "";
}

int VirtualInputs::slot_for_new() const {
  // A rule or a binding still naming a free slot would follow whatever is set up in it next.
  int fallback = -1;
  for (size_t index = 0; index < this->slots_.size(); index++) {
    if (this->slots_[index]->config.configured())
      continue;
    const SlotUsers users = this->users(index);
    if (users.rules.empty() && users.relays.empty())
      return static_cast<int>(index);
    if (fallback < 0)
      fallback = static_cast<int>(index);
  }
  return fallback;
}

SlotUsers VirtualInputs::users(size_t slot) const {
  SlotUsers users;
  if (slot >= this->slots_.size())
    return users;
  const VirtualInput &entity = this->slots_[slot]->entity;
  if (this->rule_finder_)
    users.rules = this->rule_finder_(entity.get_object_id_hash());
#ifdef VIRTUAL_INPUTS_BINDINGS
  if (this->switch_settings_ != nullptr) {
    char buffer[OBJECT_ID_MAX_LEN];
    users.relays = this->switch_settings_->relays_bound_to(std::string(entity.get_object_id_to(buffer)));
  }
#endif
  return users;
}

// --- Changing them ---

bool VirtualInputs::can_save() const { return this->keeper_ != nullptr && this->keeper_->can_save(); }

std::vector<InputConfig> VirtualInputs::table_() const {
  std::vector<InputConfig> table(this->max_inputs_);
  for (size_t index = 0; index < this->slots_.size() && index < table.size(); index++)
    table[index] = this->slots_[index]->config;
  return table;
}

bool VirtualInputs::write_(const std::vector<InputConfig> &table) {
  this->file_.set_table(table);
  this->keeper_->ensure_config_dir();
  if (this->file_.save_to_file(this->keeper_->get_storage(), this->keeper_->get_config_dir()))
    return true;
  // What the file holds is still what is in force.
  this->file_.set_table(this->table_());
  return false;
}

bool VirtualInputs::save(size_t index, const InputConfig &config) {
  if (index >= this->slots_.size() || this->check_source(config.source) != SourceCheck::OK ||
      config.on_delay > MAX_DELAY_S || config.off_delay > MAX_DELAY_S || !this->can_save() || !this->file_loaded_)
    return false;
  Slot *slot = this->slots_[index];
  if (slot->config == config)
    return true;
  std::vector<InputConfig> table = this->table_();
  table[index] = config;
  if (!this->write_(table))
    return false;
  const bool created = !slot->config.configured();
  slot->config = config;
  if (created) {
    slot->line.reset();
    slot->entity.set_visible(true, 0);
    this->schedule_ha_resync_();
  }
  this->start_(slot, this->now_ms_());
  // After the start, as at boot: to a rule built now, the level the slot took is a state, not an edge.
  if (created)
    this->rebuild_rules_(slot);
  ESP_LOGI(TAG, "%s follows '%s', on after %us, off after %us", slot->entity.fixed_name(), config.source.c_str(),
           static_cast<unsigned>(config.on_delay), static_cast<unsigned>(config.off_delay));
  return true;
}

bool VirtualInputs::remove(size_t index) {
  if (!this->configured(index) || !this->can_save() || !this->file_loaded_)
    return false;
  std::vector<InputConfig> table = this->table_();
  table[index] = InputConfig{};
  if (!this->write_(table))
    return false;
  Slot *slot = this->slots_[index];
  slot->config = InputConfig{};
  slot->source = nullptr;
  slot->line.reset();
  slot->entity.set_visible(false, 0);
  slot->entity.invalidate_state();
  // A rule naming it stops, as for any input this firmware lacks, until one is back in the slot.
  this->rebuild_rules_(slot);
  char buffer[OBJECT_ID_MAX_LEN];
  [[maybe_unused]] const std::string object_id(slot->entity.get_object_id_to(buffer));
#ifdef ENTITY_CONFIG_BINARY_SENSOR
  // The next virtual input in this slot starts without the old one's label.
  if (this->input_settings_ != nullptr && this->input_settings_->remove_record(object_id))
    this->keeper_->save(this->input_settings_->get_key());
#endif
#ifdef VIRTUAL_INPUTS_BINDINGS
  // A relay bound to it would follow the next one in this slot: its Bind to goes to None.
  if (this->switch_settings_ != nullptr && this->switch_settings_->unbind_input(object_id))
    this->keeper_->save(this->switch_settings_->get_key());
#endif
  this->schedule_ha_resync_();
  ESP_LOGI(TAG, "%s deleted", slot->entity.fixed_name());
  return true;
}

void VirtualInputs::rebuild_rules_(const Slot *slot) {
  if (this->rule_rebuilder_)
    this->rule_rebuilder_(slot->entity.get_object_id_hash());
}

// --- Home Assistant ---

void VirtualInputs::schedule_ha_resync_() {
  this->set_timeout("ha_resync", this->ha_resync_delay_ms_, [this]() { this->resync_home_assistant_(); });
}

// Upstream's own way of making clients reconnect (it does the same after a new API key). A
// client that cannot take the request is dropped, which Home Assistant answers the same way.
void VirtualInputs::resync_home_assistant_() {
#ifdef USE_API
  if (api::global_api_server == nullptr)
    return;
  ESP_LOGI(TAG, "Virtual inputs changed: asking Home Assistant to reconnect");
  for (const auto &client : api::global_api_server->active_clients()) {
    if (!client->is_connection_setup())
      continue;
    api::DisconnectRequest request;
    if (!client->send_message(request))
      client->on_fatal_error();
  }
#endif
}

}  // namespace esphome::virtual_inputs
