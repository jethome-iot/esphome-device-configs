#include "mqtt_config.h"
#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/mqtt/mqtt_sensor.h"
#endif

namespace esphome::mqtt_config {

static const char *const TAG = "mqtt_config";
// The tag upstream's ESP32 backend logs its errors under.
static const char *const CLIENT_TAG = "mqtt";

static const uint32_t CLEANUP_POLL_MS = 100;

MqttConfig *global_mqtt_config = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

MqttConfig::MqttConfig(mqtt::MQTTClientComponent *client) : client_(client) { global_mqtt_config = this; }

void MqttConfig::set_birth_template(const char *payload, uint8_t qos, bool retain) {
  this->birth_ = Template{true, payload, qos, retain};
}
void MqttConfig::set_will_template(const char *payload, uint8_t qos, bool retain) {
  this->will_ = Template{true, payload, qos, retain};
}
void MqttConfig::set_shutdown_template(const char *payload, uint8_t qos, bool retain) {
  this->shutdown_ = Template{true, payload, qos, retain};
}

void MqttConfig::add_entity(mqtt::MQTTComponent *component, const EntityBase *entity) {
  this->entities_.push_back(Walked{component, entity});
}

void MqttConfig::setup() {
  // What was compiled, before anything here changes it. The client has no getter for its
  // default id, so it is rebuilt the way its constructor builds it.
  this->compiled_discovery_ = this->client_->get_discovery_info();
  this->default_prefix_ = this->client_->get_topic_prefix();
  char mac[MAC_ADDRESS_BUFFER_SIZE];
  get_mac_address_into_buffer(mac);
  const StringRef &name = App.get_name();
  char client_id[MAX_NAME_WITH_SUFFIX_SIZE];
  const size_t len = make_name_with_suffix_to(client_id, sizeof(client_id), name.c_str(), name.size(), '-', mac,
                                              MAC_ADDRESS_BUFFER_SIZE - 1);
  this->default_client_id_.assign(client_id, len);

  this->held_back_ = this->crash_streak() >= HOLD_MQTT_AT;
  this->load_();

  // Topics apply even with MQTT off: entity components build theirs from the prefix at their
  // own setup, and a first start later in this boot keeps them. The empty check value never
  // matches, so the stored prefix is taken literally.
  if (!this->stored_.topic_prefix.empty())
    this->client_->set_topic_prefix(this->stored_.topic_prefix, "");
  this->applied_.topic_prefix = this->effective_prefix_(this->stored_);
  this->apply_status_topics_(this->applied_.topic_prefix);

  const bool start = this->stored_.enabled && !this->held_back_ && !this->stored_.broker.empty();
  if (start && this->stored_.discovery) {
    // The configs are announced again on connect, so nothing is left to remove.
    this->stored_.clean_pending = false;
  } else if (start && this->stored_.clean_pending) {
    // The fallback for a cleanup an earlier boot did not finish.
    this->restore_discovery_(true);
    this->cleanup_active_ = true;
  } else {
    this->client_->disable_discovery();
  }

  if (start) {
    // Once: esp-mqtt takes the settings when the client first connects and keeps them.
    this->push_credentials_(this->stored_);
    this->client_->set_enable_on_boot(true);
    this->started_ = true;
    const std::string prefix = this->applied_.topic_prefix;
    this->applied_ = this->stored_;
    this->applied_.topic_prefix = prefix;
    this->applied_.client_id = this->effective_client_id_(this->stored_);
  }
  this->applied_.discovery = start && this->stored_.discovery;
  if (this->held_back_ && this->stored_.enabled) {
    this->last_error_ = MqttError::CRASH_GUARD;
    ESP_LOGW(TAG, "MQTT is held back after %u crashes in a row; a restart retries it",
             static_cast<unsigned>(this->streak_));
  }

#ifdef USE_SENSOR
  this->bridge_runtime_sensors_();
#endif

  this->client_->set_on_connect([this](bool) { this->on_connect_(); });
  this->client_->set_on_disconnect([this](mqtt::MQTTClientDisconnectReason reason) { this->on_disconnect_(reason); });
  this->add_log_listener_();

  this->update_reboot_required_();
  this->refresh_state_();
  if (!this->cleanup_active_)
    this->disable_loop();
}

void MqttConfig::load_() {
  this->pref_ = global_preferences->make_preference<StoredMqttV1>(this->preference_hash_);
  StoredMqttV1 raw{};
  // A record of another size does not load at all and reads as nothing stored.
  if (!this->load_record_(raw))
    return;
  if (raw.min_reader > RECORD_LAYOUT) {
    // Newer firmware's, after a rollback: applying half of it could misread the rest.
    this->foreign_ = true;
    this->stored_notice_ = "newer_firmware";
    ESP_LOGW(TAG, "The stored MQTT settings are newer firmware's; MQTT stays off until they are saved again");
    return;
  }
  this->raw_ = raw;
  this->stored_ = from_stored(raw);
  // Applied anyway: only older firmware with looser rules can have stored it, and turning
  // MQTT off would cut the device off its broker after an update. The next save must pass.
  if (const char *error = validate(this->stored_, BUILD_HAS_IPV6); error != nullptr) {
    this->stored_notice_ = std::string("invalid: ") + error;
    ESP_LOGW(TAG, "The stored MQTT settings break a rule (%s); they apply all the same", error);
  }
}

void MqttConfig::apply_status_topics_(const std::string &prefix) {
  const std::string topic = prefix + "/status";
  if (this->birth_.present)
    this->client_->set_birth_message(
        mqtt::MQTTMessage{topic, this->birth_.payload, this->birth_.qos, this->birth_.retain});
  if (this->will_.present)
    this->client_->set_last_will(mqtt::MQTTMessage{topic, this->will_.payload, this->will_.qos, this->will_.retain});
  if (this->shutdown_.present) {
    this->client_->set_shutdown_message(
        mqtt::MQTTMessage{topic, this->shutdown_.payload, this->shutdown_.qos, this->shutdown_.retain});
  }
}

void MqttConfig::push_credentials_(const MqttRecord &record) {
  this->client_->set_broker_address(record.broker);
  this->client_->set_broker_port(static_cast<uint16_t>(record.port));
  this->client_->set_username(record.username);
  this->client_->set_password(record.password);
  // Empty keeps the client's own default.
  if (!record.client_id.empty())
    this->client_->set_client_id(record.client_id);
}

void MqttConfig::restore_discovery_(bool clean) {
  const mqtt::MQTTDiscoveryInfo &compiled = this->compiled_discovery_;
  this->client_->set_discovery_info(std::string(compiled.prefix), compiled.unique_id_generator,
                                    compiled.object_id_generator, compiled.retain, compiled.discover_ip, clean);
}

const std::string &MqttConfig::effective_prefix_(const MqttRecord &record) const {
  return record.topic_prefix.empty() ? this->default_prefix_ : record.topic_prefix;
}

const std::string &MqttConfig::effective_client_id_(const MqttRecord &record) const {
  return record.client_id.empty() ? this->default_client_id_ : record.client_id;
}

// --- Later changes ---

MqttConfig::UpdateResult MqttConfig::update(const MqttPatch &patch) {
  MqttRecord merged = this->stored_;
  patch.apply_to(merged);
  if (merged.same_settings(this->stored_))
    return this->result_(200, "Nothing changed", false);
  if (const char *error = validate(merged, BUILD_HAS_IPV6); error != nullptr)
    return this->result_(400, error, false);

  // Retained configs go now from the broker that has them: the config topic does not depend
  // on the prefix, the client id or the credentials, so only these changes call for it.
  const bool discovery_live = this->started_ && this->applied_.discovery;
  const bool clean_now =
      discovery_live && (!merged.discovery || !merged.enabled || merged.broker != this->applied_.broker ||
                         merged.port != this->applied_.port);
  if (merged.enabled && merged.discovery) {
    merged.clean_pending = false;  // announced again at the next connect
  } else if (clean_now) {
    merged.clean_pending = true;  // until a connect has finished it
  }
  // Command topics were subscribed with the boot's prefix, so a new one waits for a restart.
  const bool start_now = !this->started_ && merged.enabled && !this->held_back_ &&
                         this->effective_prefix_(merged) == this->applied_.topic_prefix;

  // Stored before applied, so a failed write leaves the device as it was.
  const StoredMqttV1 out = to_stored(merged, this->foreign_ ? StoredMqttV1{} : this->raw_);
  if (!this->store_(out)) {
    // The flush reports for every key at once, so the failure may be another record's.
    if (!this->flash_holds_(out)) {
      ESP_LOGE(TAG, "Storing the MQTT settings failed; the old ones stay in force");
      return this->result_(500, "Storing the MQTT settings failed; the old ones stay in force", false);
    }
    ESP_LOGW(TAG, "Flushing flash failed for another record; the MQTT settings are stored all the same");
  }
  this->raw_ = out;
  this->foreign_ = false;
  this->stored_notice_.clear();
  this->stored_ = merged;

  if (clean_now) {
    this->start_cleanup_();
    this->applied_.discovery = false;  // off applies now, so it waits for no restart
  }
  if (start_now) {
    if (merged.discovery) {
      this->restore_discovery_(false);
    } else if (merged.clean_pending) {
      this->start_cleanup_();  // finishes what an earlier "MQTT off" left
    } else {
      this->client_->disable_discovery();
    }
    this->push_credentials_(merged);
    this->client_->enable();
    this->started_ = true;
    this->applied_ = merged;
    this->applied_.topic_prefix = this->effective_prefix_(merged);
    this->applied_.client_id = this->effective_client_id_(merged);
    ESP_LOGI(TAG, "MQTT turned on: connecting to %s:%u", merged.broker.c_str(), static_cast<unsigned>(merged.port));
  }
  this->update_reboot_required_();
  this->refresh_state_();
  return this->result_(200, this->save_message_(start_now, merged.enabled), start_now);
}

const char *MqttConfig::save_message_(bool started, bool enabled) const {
  const DiscoveryCleanup cleanup = this->discovery_cleanup();
  const bool reboot = this->reboot_required_.load();
  if (started)
    return "Saved; MQTT is connecting";
  if (reboot && cleanup == DiscoveryCleanup::RUNNING)
    return "Saved; removing this device's Home Assistant entries now, the rest applies after a reboot";
  if (reboot && cleanup == DiscoveryCleanup::PENDING && !enabled) {
    return "Saved; applies after a reboot. The broker is not reachable, so this device's Home Assistant entries "
           "stay on it unless it comes back before then";
  }
  if (reboot)
    return "Saved; applies after a reboot";
  if (cleanup == DiscoveryCleanup::RUNNING)
    return "Saved; removing this device's Home Assistant entries";
  if (cleanup == DiscoveryCleanup::PENDING)
    return "Saved; this device's Home Assistant entries go once the broker is reachable";
  return "Saved";
}

MqttConfig::UpdateResult MqttConfig::result_(int code, const char *message, bool started) const {
  return UpdateResult{code, message, started, this->reboot_required_.load(), this->discovery_cleanup()};
}

bool MqttConfig::load_record_(StoredMqttV1 &out) { return this->pref_.load(&out); }

// save() only queues the record; sync() is what reaches NVS, so it is the call that can fail.
bool MqttConfig::store_(const StoredMqttV1 &stored) { return this->pref_.save(&stored) && global_preferences->sync(); }

// Byte for byte: the record has no padding, so what was written compares equal to nothing else.
bool MqttConfig::flash_holds_(const StoredMqttV1 &stored) {
  StoredMqttV1 readback{};
  return this->load_record_(readback) && std::memcmp(&readback, &stored, sizeof(StoredMqttV1)) == 0;
}

void MqttConfig::update_reboot_required_() {
  bool required;
  if (this->foreign_) {
    required = false;
  } else if (this->started_) {
    required = !same_running_settings(this->stored_, this->applied_, this->default_client_id_, this->default_prefix_);
  } else {
    // A held-back client counts: a restart is exactly what clears the guard.
    required = this->stored_.enabled &&
               (this->held_back_ || this->effective_prefix_(this->stored_) != this->applied_.topic_prefix);
  }
  this->reboot_required_ = required;
}

// --- Discovery cleanup ---

void MqttConfig::start_cleanup_() {
  this->restore_discovery_(true);
  // Internal components never registered with the client, so nothing would process theirs.
  for (const Walked &walked : this->entities_) {
    if (!walked.component->is_internal())
      walked.component->schedule_resend_state();
  }
  this->cleanup_active_ = true;
  this->enable_loop();
}

// Done once the client is connected and no component waits for its resend. is_connected()
// turns true in the same client pass that schedules the resends, so "connected, nothing
// pending" is never seen before they exist.
void MqttConfig::check_cleanup_() {
  if (!this->cleanup_active_ || !this->client_->is_connected())
    return;
  for (const Walked &walked : this->entities_) {
    if (!walked.component->is_internal() && walked.component->is_resend_pending())
      return;
  }
  this->finish_cleanup_();
}

// Clean mode stays on for the rest of the boot: every later resend clears again.
void MqttConfig::finish_cleanup_() {
  this->cleanup_active_ = false;
  ESP_LOGI(TAG, "Removed this device's Home Assistant entries from the broker");
  if (this->stored_.clean_pending) {
    MqttRecord cleared = this->stored_;
    cleared.clean_pending = false;
    const StoredMqttV1 out = to_stored(cleared, this->raw_);
    if (this->store_(out) || this->flash_holds_(out)) {
      this->raw_ = out;
      this->stored_ = cleared;
    } else {
      ESP_LOGW(TAG, "Could not store that the cleanup finished; the next boot repeats it");
    }
  }
  this->disable_loop();
}

void MqttConfig::loop() {
  this->check_cleanup_();
  if (!this->cleanup_active_)
    this->disable_loop();
}

DiscoveryCleanup MqttConfig::discovery_cleanup() const {
  if (this->cleanup_active_)
    return this->client_->is_connected() ? DiscoveryCleanup::RUNNING : DiscoveryCleanup::PENDING;
  if (this->stored_.clean_pending)
    return DiscoveryCleanup::PENDING;
  return DiscoveryCleanup::NONE;
}

void MqttConfig::after_cleanup(uint32_t timeout_ms, std::function<void()> &&then) {
  if (this->discovery_cleanup() != DiscoveryCleanup::RUNNING) {
    then();
    return;
  }
  this->waiters_.push_back(Waiter{std::move(then), millis(), timeout_ms});
  this->set_interval("mqtt-cleanup", CLEANUP_POLL_MS, [this]() { this->poll_waiters_(); });
}

void MqttConfig::poll_waiters_() {
  this->check_cleanup_();
  const bool running = this->discovery_cleanup() == DiscoveryCleanup::RUNNING;
  const uint32_t now = millis();
  std::vector<std::function<void()>> due;
  for (auto it = this->waiters_.begin(); it != this->waiters_.end();) {
    if (!running || now - it->since >= it->timeout_ms) {
      due.push_back(std::move(it->then));
      it = this->waiters_.erase(it);
    } else {
      ++it;
    }
  }
  if (this->waiters_.empty())
    this->cancel_interval("mqtt-cleanup");
  // Last: a reboot does not come back.
  for (auto &then : due)
    then();
}

void MqttConfig::before_factory_reset(std::function<void()> &&then) {
  if (this->started_ && this->applied_.discovery && this->client_->is_connected()) {
    // The record goes with NVS, so nothing after the reset could remove the entries.
    this->start_cleanup_();
    this->applied_.discovery = false;
    this->update_reboot_required_();
  }
  this->after_cleanup(5000, std::move(then));
}

// --- Runtime sensors ---

bool MqttConfig::serves_(const EntityBase *entity) const {
  return std::any_of(this->entities_.begin(), this->entities_.end(),
                     [entity](const Walked &walked) { return walked.entity == entity; });
}

#ifdef USE_SENSOR
// Codegen gives an MQTT component only to entities it declares; these get one here, set up
// as the client's own children are, and join the cleanup walk.
void MqttConfig::bridge_runtime_sensors_() {
  for (const SensorSource &source : this->sensor_sources_) {
    for (sensor::Sensor *sensor : source()) {
      if (sensor == nullptr || this->serves_(sensor))
        continue;
      auto *bridge = new mqtt::MQTTSensorComponent(sensor);  // NOLINT(cppcoreguidelines-owning-memory)
      bridge->call_setup();
      this->entities_.push_back(Walked{bridge, sensor});
    }
  }
}
#endif

// --- Status ---

void MqttConfig::on_connect_() {
  this->connected_ = true;
  this->last_error_ = MqttError::NONE;
  this->pending_error_ = MqttError::NONE;
  // Armed until the connection has lasted long enough to prove the client harmless.
  this->guard_record_().armed = 1;
  this->set_timeout("mqtt-guard", this->disarm_after_ms_, [this]() {
    CrashGuardRecord &record = this->guard_record_();
    record.streak = 0;
    record.armed = 0;
  });
  this->refresh_state_();
}

void MqttConfig::on_disconnect_(mqtt::MQTTClientDisconnectReason reason) {
  using Reason = mqtt::MQTTClientDisconnectReason;
  const bool was_connected = this->connected_.exchange(false);
  this->cancel_timeout("mqtt-guard");
  MqttError error;
  switch (reason) {
    case Reason::DNS_RESOLVE_ERROR:
      error = MqttError::DNS;
      break;
    case Reason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
      error = MqttError::PROTOCOL;
      break;
    case Reason::MQTT_IDENTIFIER_REJECTED:
      error = MqttError::IDENTIFIER_REJECTED;
      break;
    case Reason::MQTT_SERVER_UNAVAILABLE:
      error = MqttError::SERVER_UNAVAILABLE;
      break;
    case Reason::MQTT_MALFORMED_CREDENTIALS:
      error = MqttError::BAD_CREDENTIALS;
      break;
    case Reason::MQTT_NOT_AUTHORIZED:
      error = MqttError::NOT_AUTHORIZED;
      break;
    default:
      // The ESP32 backend reports every drop as a TCP one; the cause, if any, was logged just
      // before it.
      if (was_connected) {
        error = MqttError::CONNECTION_LOST;
      } else if (const MqttError logged = this->pending_error_.load(); logged != MqttError::NONE) {
        error = logged;
      } else {
        error = MqttError::UNREACHABLE;
      }
      break;
  }
  this->pending_error_ = MqttError::NONE;
  this->last_error_ = error;
  this->refresh_state_();
}

void MqttConfig::add_log_listener_() {
#ifdef USE_LOGGER
  if (logger::global_logger != nullptr)
    logger::global_logger->add_log_callback(this, &MqttConfig::on_log_);
#endif
}

// Matches the backend's two error lines; if upstream rewords them, a refusal reads as
// unreachable. Called for every line logged, so it returns early.
void MqttConfig::on_log_(void *self, uint8_t level, const char *tag, const char *message, size_t len) {
  if (level != ESPHOME_LOG_LEVEL_ERROR || tag == nullptr || std::strcmp(tag, CLIENT_TAG) != 0)
    return;
  auto *config = static_cast<MqttConfig *>(self);
  const std::string_view text(message, len);
  static constexpr std::string_view REFUSED = "Connection refused error: 0x";
  if (const size_t at = text.find(REFUSED); at != std::string_view::npos) {
    unsigned code = 0;
    for (size_t i = at + REFUSED.size(); i < text.size(); i++) {
      const char c = text[i];
      int digit;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        break;
      }
      code = code * 16 + static_cast<unsigned>(digit);
      if (code > 0xFF)
        break;
    }
    static constexpr MqttError BY_CODE[] = {MqttError::PROTOCOL, MqttError::IDENTIFIER_REJECTED,
                                            MqttError::SERVER_UNAVAILABLE, MqttError::BAD_CREDENTIALS,
                                            MqttError::NOT_AUTHORIZED};
    if (code >= 1 && code <= 5)
      config->pending_error_ = BY_CODE[code - 1];
  } else if (text.find("socket errno:") != std::string_view::npos) {
    config->pending_error_ = MqttError::UNREACHABLE;
  }
}

void MqttConfig::refresh_state_() {
  MqttState state;
  if (!this->started_) {
    state = this->stored_.broker.empty() ? MqttState::NOT_CONFIGURED : MqttState::OFF;
  } else if (this->connected_) {
    state = MqttState::CONNECTED;
  } else if (this->last_error_ == MqttError::NONE) {
    state = MqttState::CONNECTING;
  } else {
    state = MqttState::DISCONNECTED;
  }
  this->state_ = state;
}

MqttConfig::LiveStatus MqttConfig::live_status() const {
  return LiveStatus{this->started_.load(), this->connected_.load(), this->state_.load(), this->last_error_.load()};
}

uint8_t MqttConfig::crash_streak() {
  if (!this->guard_evaluated_) {
    CrashGuardRecord &record = this->guard_record_();
    this->streak_ = next_streak(record, record.magic == CRASH_GUARD_MAGIC, this->panic_reset_());
    record.magic = CRASH_GUARD_MAGIC;
    record.streak = this->streak_;
    record.armed = 0;
    this->guard_evaluated_ = true;
  }
  return this->streak_;
}

void MqttConfig::write_settings_json(JsonObject root) const {
  const MqttRecord &s = this->stored_;
  root["enabled"] = s.enabled;
  root["broker"] = s.broker;
  root["port"] = s.port;
  root["username"] = s.username;
  root["password_set"] = !s.password.empty();
  root["client_id"] = s.client_id;
  root["client_id_default"] = this->default_client_id_;
  root["topic_prefix"] = s.topic_prefix;
  root["topic_prefix_default"] = this->default_prefix_;
  root["discovery"] = s.discovery;
  root["state"] = state_key(this->state_.load());
  if (const char *error = error_key(this->last_error_.load()); error != nullptr) {
    root["last_error"] = error;
  } else {
    root["last_error"] = nullptr;
  }
  if (this->started_) {
    JsonObject running = root["running"].to<JsonObject>();
    running["broker"] = this->applied_.broker;
    running["port"] = this->applied_.port;
    running["client_id"] = this->applied_.client_id;
    running["topic_prefix"] = this->applied_.topic_prefix;
    running["status_topic"] = this->applied_.topic_prefix + "/status";
    running["discovery"] = this->applied_.discovery;
  } else {
    root["running"] = nullptr;
  }
  root["apply_now"] = !this->started_ && !this->held_back_;
  root["reboot_required"] = this->reboot_required_.load();
  root["discovery_cleanup"] = cleanup_key(this->discovery_cleanup());
  if (this->stored_notice_.empty()) {
    root["stored_notice"] = nullptr;
  } else {
    root["stored_notice"] = this->stored_notice_;
  }
}

void MqttConfig::dump_config() {
  const MqttRecord &s = this->stored_;
  ESP_LOGCONFIG(TAG,
                "MQTT settings:\n"
                "  Enabled: %s\n"
                "  Broker: %s:%u\n"
                "  Username: " LOG_SECRET("'%s'") "\n"
                                                  "  Password: %s\n"
                                                  "  Client ID: %s\n"
                                                  "  Topic prefix: %s\n"
                                                  "  Home Assistant discovery: %s\n"
                                                  "  Running: %s",
                YESNO(s.enabled), s.broker.c_str(), static_cast<unsigned>(s.port), s.username.c_str(),
                s.password.empty() ? "not set" : "set", this->effective_client_id_(s).c_str(),
                this->effective_prefix_(s).c_str(), ONOFF(s.discovery), YESNO(this->started_.load()));
  if (!this->stored_notice_.empty())
    ESP_LOGCONFIG(TAG, "  Stored notice: %s", this->stored_notice_.c_str());
  if (this->held_back_)
    ESP_LOGCONFIG(TAG, "  Held back after %u crashes in a row", static_cast<unsigned>(this->streak_));
}

const char *MqttConfig::state_key(MqttState state) {
  switch (state) {
    case MqttState::NOT_CONFIGURED:
      return "not_configured";
    case MqttState::OFF:
      return "off";
    case MqttState::CONNECTING:
      return "connecting";
    case MqttState::CONNECTED:
      return "connected";
    case MqttState::DISCONNECTED:
      return "disconnected";
  }
  return "off";
}

const char *MqttConfig::error_key(MqttError error) {
  switch (error) {
    case MqttError::NONE:
      return nullptr;
    case MqttError::DNS:
      return "dns";
    case MqttError::UNREACHABLE:
      return "unreachable";
    case MqttError::CONNECTION_LOST:
      return "connection_lost";
    case MqttError::PROTOCOL:
      return "protocol";
    case MqttError::IDENTIFIER_REJECTED:
      return "identifier_rejected";
    case MqttError::SERVER_UNAVAILABLE:
      return "server_unavailable";
    case MqttError::BAD_CREDENTIALS:
      return "bad_credentials";
    case MqttError::NOT_AUTHORIZED:
      return "not_authorized";
    case MqttError::CRASH_GUARD:
      return "crash_guard";
  }
  return nullptr;
}

const char *MqttConfig::error_label(MqttError error) {
  switch (error) {
    case MqttError::NONE:
      return "";
    case MqttError::DNS:
      return "Broker name not found";
    case MqttError::UNREACHABLE:
      return "Broker unreachable";
    case MqttError::CONNECTION_LOST:
      return "Connection lost";
    case MqttError::PROTOCOL:
      return "Protocol version refused";
    case MqttError::IDENTIFIER_REJECTED:
      return "Client ID refused";
    case MqttError::SERVER_UNAVAILABLE:
      return "Broker unavailable";
    case MqttError::BAD_CREDENTIALS:
      return "Wrong username or password";
    case MqttError::NOT_AUTHORIZED:
      return "Not authorized (check username and password)";
    case MqttError::CRASH_GUARD:
      return "Held back after repeated crashes";
  }
  return "";
}

const char *MqttConfig::cleanup_key(DiscoveryCleanup cleanup) {
  switch (cleanup) {
    case DiscoveryCleanup::NONE:
      return "none";
    case DiscoveryCleanup::RUNNING:
      return "running";
    case DiscoveryCleanup::PENDING:
      return "pending";
  }
  return "none";
}

}  // namespace esphome::mqtt_config
