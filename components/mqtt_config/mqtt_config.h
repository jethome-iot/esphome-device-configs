#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "esphome/components/json/json_util.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/components/mqtt/mqtt_component.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/preferences.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#include "crash_guard.h"
#include "mqtt_record.h"

namespace esphome::mqtt_config {

// `network` defines it with a value, false on a build without IPv6, so never #ifdef.
#if USE_NETWORK_IPV6
inline constexpr bool BUILD_HAS_IPV6 = true;
#else
inline constexpr bool BUILD_HAS_IPV6 = false;
#endif

enum class MqttState : uint8_t { NOT_CONFIGURED, OFF, CONNECTING, CONNECTED, DISCONNECTED };
enum class MqttError : uint8_t {
  NONE,
  DNS,
  UNREACHABLE,
  CONNECTION_LOST,
  PROTOCOL,
  IDENTIFIER_REJECTED,
  SERVER_UNAVAILABLE,
  BAD_CREDENTIALS,
  NOT_AUTHORIZED,
  CRASH_GUARD,
};
// Removing this device's retained Home Assistant configs: RUNNING while they go out, PENDING
// while one is due and the broker is not connected.
enum class DiscoveryCleanup : uint8_t { NONE, RUNNING, PENDING };

// The MQTT client's settings, kept in NVS and set from the dashboard. The stock client is
// compiled idle; this applies the stored record at boot, starts the client the first time it
// is turned on in a boot, and keeps every later change for the next one.
class MqttConfig : public Component {
 public:
  explicit MqttConfig(mqtt::MQTTClientComponent *client);

  // Below BEFORE_CONNECTION and above the client (AFTER_WIFI) and the entity components it
  // configures (AFTER_CONNECTION); after dallas_scan (DATA), whose sensors it bridges.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI + 10.0f; }
  void setup() override;
  // Runs only while a discovery cleanup does.
  void loop() override;
  void dump_config() override;

  // --- Codegen ---
  void set_preference_hash(uint32_t hash) { this->preference_hash_ = hash; }
  // The payloads of the availability messages; the topic is `<prefix>/status` of each device.
  void set_birth_template(const char *payload, uint8_t qos, bool retain);
  void set_will_template(const char *payload, uint8_t qos, bool retain);
  void set_shutdown_template(const char *payload, uint8_t qos, bool retain);
  // Every entity's MQTT component, hidden ones included, with the entity it serves.
  void reserve_entities(size_t count) { this->entities_.reserve(count); }
  void add_entity(mqtt::MQTTComponent *component, const EntityBase *entity);
#ifdef USE_SENSOR
  // Sensors created at run time (dallas_scan's Temp N), which codegen gave no MQTT component.
  using SensorSource = std::function<std::vector<sensor::Sensor *>()>;
  void add_runtime_sensors(SensorSource &&source) { this->sensor_sources_.push_back(std::move(source)); }
#endif

  // --- Device API, loop task ---
  struct UpdateResult {
    int code;
    const char *message;
    bool started;
    bool reboot_required;
    DiscoveryCleanup cleanup;
  };
  // Stores a POST's change and applies what can apply now. 200, 400 (a validate() message) or 500.
  UpdateResult update(const MqttPatch &patch);
  // GET /mqtt: the stored settings, never the password, and what runs this boot.
  void write_settings_json(JsonObject root) const;

  // --- Any task: atomics only ---
  struct LiveStatus {
    bool running;
    bool connected;
    MqttState state;
    MqttError last_error;
  };
  LiveStatus live_status() const;
  // A saved change waits for a restart, or the client is held back and a restart retries it.
  bool reboot_required() const { return this->reboot_required_.load(); }

  // --- Loop task ---
  MqttState state() const { return this->state_.load(); }
  MqttError last_error() const { return this->last_error_.load(); }
  // The client was started this boot.
  bool running() const { return this->started_.load(); }
  bool connected() const { return this->connected_.load(); }
  DiscoveryCleanup discovery_cleanup() const;
  // This boot's crash streak, evaluated on the first call, whoever makes it.
  uint8_t crash_streak();
  mqtt::MQTTClientComponent *client() const { return this->client_; }
  // Runs `then` once a running cleanup has finished, or after `timeout_ms`; at once when none runs.
  void after_cleanup(uint32_t timeout_ms, std::function<void()> &&then);
  // Starts a cleanup when Home Assistant has this device's entries now, then after_cleanup().
  void before_factory_reset(std::function<void()> &&then);

  static const char *state_key(MqttState state);
  // nullptr for NONE.
  static const char *error_key(MqttError error);
  // "" for NONE.
  static const char *error_label(MqttError error);
  static const char *cleanup_key(DiscoveryCleanup cleanup);

 protected:
  struct Template {
    bool present{false};
    std::string payload;
    uint8_t qos{0};
    bool retain{false};
  };
  struct Walked {
    mqtt::MQTTComponent *component;
    const EntityBase *entity;
  };
  struct Waiter {
    std::function<void()> then;
    uint32_t since;
    uint32_t timeout_ms;
  };

  // The record in NVS; false when there is none of this size. Virtual, as store_ is: the host
  // backend keeps nothing over 255 bytes, so the tests stand in for NVS.
  virtual bool load_record_(StoredMqttV1 &out);
  // Puts the record in NVS and flushes it; false when the flush reported a failure.
  virtual bool store_(const StoredMqttV1 &stored);
  // Whether NVS holds that record now, read back.
  bool flash_holds_(const StoredMqttV1 &stored);
  virtual bool panic_reset_() const { return reset_was_panic(); }
  virtual CrashGuardRecord &guard_record_() { return rtc_guard_record(); }
  // The logger keeps the pointer it is given for good, so the host tests override this.
  virtual void add_log_listener_();
  static void on_log_(void *self, uint8_t level, const char *tag, const char *message, size_t len);

  void load_();
  void apply_status_topics_(const std::string &prefix);
  void push_credentials_(const MqttRecord &record);
  void restore_discovery_(bool clean);
  void start_cleanup_();
  void check_cleanup_();
  void finish_cleanup_();
  void poll_waiters_();
#ifdef USE_SENSOR
  void bridge_runtime_sensors_();
#endif
  bool serves_(const EntityBase *entity) const;
  void on_connect_();
  void on_disconnect_(mqtt::MQTTClientDisconnectReason reason);
  void refresh_state_();
  void update_reboot_required_();
  const std::string &effective_prefix_(const MqttRecord &record) const;
  const std::string &effective_client_id_(const MqttRecord &record) const;
  const char *save_message_(bool started, bool enabled) const;
  UpdateResult result_(int code, const char *message, bool started) const;

  mqtt::MQTTClientComponent *client_;
  ESPPreferenceObject pref_;
  uint32_t preference_hash_{0};

  MqttRecord stored_;    // what NVS holds; the defaults while it holds newer firmware's record
  MqttRecord applied_;   // what runs: the topic prefix always, the rest once started, with the defaults filled in
  StoredMqttV1 raw_{};   // as loaded, for the flag bits and bytes this layout does not know
  bool foreign_{false};  // newer firmware's record: never written but on an explicit save
  std::string stored_notice_;
  bool held_back_{false};
  bool cleanup_active_{false};

  std::atomic<bool> started_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> reboot_required_{false};
  std::atomic<MqttState> state_{MqttState::NOT_CONFIGURED};
  std::atomic<MqttError> last_error_{MqttError::NONE};
  std::atomic<MqttError> pending_error_{MqttError::NONE};  // read from the log, waiting for the drop

  bool guard_evaluated_{false};
  uint8_t streak_{0};
  uint32_t disarm_after_ms_{DISARM_AFTER_MS};

  mqtt::MQTTDiscoveryInfo compiled_discovery_{};
  std::string default_prefix_;
  std::string default_client_id_;
  Template birth_;
  Template will_;
  Template shutdown_;
  std::vector<Walked> entities_;
#ifdef USE_SENSOR
  std::vector<SensorSource> sensor_sources_;
#endif
  std::vector<Waiter> waiters_;
};

extern MqttConfig *global_mqtt_config;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::mqtt_config
