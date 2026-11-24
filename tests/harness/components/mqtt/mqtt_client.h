#pragma once

// Host stand-in for upstream's MQTTClientComponent, which builds for ESP platforms and LibreTiny
// only. It keeps upstream's setters, getters and the rules a component under test depends on
// (the topic prefix, discovery info, availability, additive callbacks, the resend pass), and
// replaces the broker with drivers a test calls. Nothing opens a socket.

#include "esphome/core/defines.h"

#ifdef USE_MQTT

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "esphome/components/json/json_util.h"
#include "esphome/core/application.h"
#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"
#include "mqtt_backend.h"

namespace esphome::mqtt {

using mqtt_on_connect_callback_t = std::function<void(bool session_present)>;
using mqtt_on_disconnect_callback_t = std::function<void(MQTTClientDisconnectReason reason)>;
using mqtt_callback_t = std::function<void(const std::string &, const std::string &)>;
using mqtt_json_callback_t = std::function<void(const std::string &, JsonObject)>;

struct MQTTSubscription {
  std::string topic;
  uint8_t qos;
  mqtt_callback_t callback;
};

struct MQTTCredentials {
  std::string address;
  uint16_t port;
  std::string username;
  std::string password;
  std::string client_id;
  bool clean_session;
};

struct Availability {
  std::string topic;
  std::string payload_available;
  std::string payload_not_available;
};

enum MQTTDiscoveryUniqueIdGenerator {
  MQTT_LEGACY_UNIQUE_ID_GENERATOR = 0,
  MQTT_MAC_ADDRESS_UNIQUE_ID_GENERATOR,
};

enum MQTTDiscoveryObjectIdGenerator {
  MQTT_NONE_OBJECT_ID_GENERATOR = 0,
  MQTT_DEVICE_NAME_OBJECT_ID_GENERATOR,
};

struct MQTTDiscoveryInfo {
  std::string prefix;
  bool retain;
  bool discover_ip;
  bool clean;
  MQTTDiscoveryUniqueIdGenerator unique_id_generator;
  MQTTDiscoveryObjectIdGenerator object_id_generator;
};

class MQTTComponent;
class MQTTClientComponent;

inline MQTTClientComponent *global_mqtt_client = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Upstream's topic_match (mqtt_client.cpp), in its order: `+` and `#` match, except at the top
// level of a `$` topic, and an exhausted topic fails before `#` is looked at, so `a/#` misses
// `a` there although MQTT 3.1.1 counts zero levels. The stand-in keeps that, as the device does.
inline bool topic_match(const char *message, const char *subscription, bool is_normal, bool past_separator) {
  if (*message == '\0' && *subscription == '\0')
    return true;
  if (*message == '\0' || *subscription == '\0')
    return false;
  const bool do_wildcards = is_normal || past_separator;
  if (*subscription == '+' && do_wildcards) {
    subscription++;
    while (*message != '\0' && *message != '/')
      message++;
    return topic_match(message, subscription, is_normal, true);
  }
  if (*subscription == '#' && do_wildcards)
    return true;
  if (*message != *subscription)
    return false;
  past_separator = past_separator || *subscription == '/';
  return topic_match(message + 1, subscription + 1, is_normal, past_separator);
}

inline bool topic_match(const char *message, const char *subscription) {
  return topic_match(message, subscription, *message != '\0' && *message != '$', false);
}

class MQTTClientComponent : public Component {
 public:
  // What a publish carried, in the order the client was asked.
  struct Published {
    std::string topic;
    std::string payload;
    uint8_t qos;
    bool retain;
  };

  // Upstream's default client id: the node name, '-', the MAC.
  MQTTClientComponent() {
    global_mqtt_client = this;
    char mac[MAC_ADDRESS_BUFFER_SIZE];
    get_mac_address_into_buffer(mac);
    const StringRef &name = App.get_name();
    char client_id[MAX_NAME_WITH_SUFFIX_SIZE];
    const size_t len = make_name_with_suffix_to(client_id, sizeof(client_id), name.c_str(), name.size(), '-', mac,
                                                MAC_ADDRESS_BUFFER_SIZE - 1);
    this->credentials_.client_id.assign(client_id, len);
  }

  void set_last_will(MQTTMessage &&message) {
    this->last_will_ = std::move(message);
    this->recalculate_availability_();
  }
  // Upstream's (mqtt_client.cpp) leaves the availability as it was, unlike the other two.
  void disable_last_will() { this->last_will_.topic = ""; }
  void set_birth_message(MQTTMessage &&message) {
    this->birth_message_ = std::move(message);
    this->recalculate_availability_();
  }
  void disable_birth_message() {
    this->birth_message_.topic = "";
    this->recalculate_availability_();
  }
  void set_shutdown_message(MQTTMessage &&message) { this->shutdown_message_ = std::move(message); }
  void disable_shutdown_message() {
    this->shutdown_message_.topic = "";
    this->recalculate_availability_();
  }
  void set_keep_alive(uint16_t keep_alive_s) { this->keep_alive_s_ = keep_alive_s; }

  void set_discovery_info(std::string &&prefix, MQTTDiscoveryUniqueIdGenerator unique_id_generator,
                          MQTTDiscoveryObjectIdGenerator object_id_generator, bool retain, bool discover_ip,
                          bool clean = false) {
    this->discovery_info_.prefix = std::move(prefix);
    this->discovery_info_.discover_ip = discover_ip;
    this->discovery_info_.unique_id_generator = unique_id_generator;
    this->discovery_info_.object_id_generator = object_id_generator;
    this->discovery_info_.retain = retain;
    this->discovery_info_.clean = clean;
    this->discovery_info_calls_++;
  }
  const MQTTDiscoveryInfo &get_discovery_info() const { return this->discovery_info_; }
  void disable_discovery() {
    this->discovery_info_ = MQTTDiscoveryInfo{
        .prefix = "",
        .retain = false,
        .discover_ip = false,
        .clean = false,
        .unique_id_generator = MQTT_LEGACY_UNIQUE_ID_GENERATOR,
        .object_id_generator = MQTT_NONE_OBJECT_ID_GENERATOR,
    };
    this->disable_discovery_calls_++;
  }
  bool is_discovery_enabled() const { return !this->discovery_info_.prefix.empty(); }
  bool is_discovery_ip_enabled() const { return this->discovery_info_.discover_ip; }
  const Availability &get_availability() { return this->availability_; }

  // Upstream's rule: with the MAC suffix on, a prefix equal to the check value becomes the
  // sanitized node name; anything else is taken literally.
  void set_topic_prefix(const std::string &topic_prefix, const std::string &check_topic_prefix) {
    if (App.is_name_add_mac_suffix_enabled() && topic_prefix == check_topic_prefix) {
      char buf[ESPHOME_DEVICE_NAME_MAX_LEN + 1];
      this->topic_prefix_ = str_sanitize_to(buf, App.get_name().c_str());
    } else {
      this->topic_prefix_ = topic_prefix;
    }
  }
  const std::string &get_topic_prefix() const { return this->topic_prefix_; }

  void set_log_message_template(MQTTMessage &&message) { this->log_message_ = std::move(message); }
  void set_log_level(int level) { this->log_level_ = level; }
  void disable_log_message() { this->log_message_.topic = ""; }
  bool is_log_message_enabled() const { return !this->log_message_.topic.empty(); }

  void subscribe(const std::string &topic, mqtt_callback_t callback, uint8_t qos = 0) {
    this->subscriptions_.push_back(MQTTSubscription{topic, qos, std::move(callback)});
  }
  void subscribe_json(const std::string &topic, const mqtt_json_callback_t &callback, uint8_t qos = 0) {
    this->subscribe(
        topic,
        [callback](const std::string &t, const std::string &payload) {
          json::parse_json(payload, [&t, &callback](JsonObject root) -> bool {
            callback(t, root);
            return true;
          });
        },
        qos);
  }
  void unsubscribe(const std::string &topic) {
    for (auto it = this->subscriptions_.begin(); it != this->subscriptions_.end();) {
      it = it->topic == topic ? this->subscriptions_.erase(it) : it + 1;
    }
  }

  // As upstream: nothing goes out while disconnected, and the caller is told so.
  bool publish(const char *topic, const char *payload, size_t payload_length, uint8_t qos = 0, bool retain = false) {
    if (!this->is_connected() || this->fail_publishes)
      return false;
    this->published.push_back(Published{topic, std::string(payload, payload_length), qos, retain});
    return true;
  }
  bool publish(const std::string &topic, const std::string &payload, uint8_t qos = 0, bool retain = false) {
    return this->publish(topic.c_str(), payload.data(), payload.size(), qos, retain);
  }
  bool publish(const std::string &topic, const char *payload, size_t payload_length, uint8_t qos = 0,
               bool retain = false) {
    return this->publish(topic.c_str(), payload, payload_length, qos, retain);
  }
  bool publish(const MQTTMessage &message) {
    return this->publish(message.topic.c_str(), message.payload.c_str(), message.payload.size(), message.qos,
                         message.retain);
  }
  bool publish_json(const char *topic, const json::json_build_t &f, uint8_t qos = 0, bool retain = false) {
    auto message = json::build_json(f);
    return this->publish(topic, message.c_str(), message.size(), qos, retain);
  }
  bool publish_json(const std::string &topic, const json::json_build_t &f, uint8_t qos = 0, bool retain = false) {
    return this->publish_json(topic.c_str(), f, qos, retain);
  }

  void setup() override {
    if (this->enable_on_boot_)
      this->enable();
  }
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_reboot_timeout(uint32_t reboot_timeout) { this->reboot_timeout_ = reboot_timeout; }
  void register_mqtt_component(MQTTComponent *component) { this->children_.push_back(component); }

  bool is_connected() const { return this->connected_; }
  void set_enable_on_boot(bool enable_on_boot) { this->enable_on_boot_ = enable_on_boot; }
  void enable() {
    this->enable_calls++;
    this->enabled_ = true;
  }
  void disable() {
    this->disable_calls++;
    this->enabled_ = false;
  }

  void set_broker_address(const std::string &address) { this->credentials_.address = address; }
  void set_broker_port(uint16_t port) { this->credentials_.port = port; }
  void set_username(const std::string &username) { this->credentials_.username = username; }
  void set_password(const std::string &password) { this->credentials_.password = password; }
  void set_client_id(const std::string &client_id) { this->credentials_.client_id = client_id; }
  void set_clean_session(const bool &clean_session) { this->credentials_.clean_session = clean_session; }
  // Additive, as upstream's: every caller's callback runs.
  void set_on_connect(mqtt_on_connect_callback_t &&callback) { this->on_connect_.push_back(std::move(callback)); }
  void set_on_disconnect(mqtt_on_disconnect_callback_t &&callback) {
    this->on_disconnect_.push_back(std::move(callback));
  }

  void set_publish_nan_as_none(bool publish_nan_as_none) { this->publish_nan_as_none_ = publish_nan_as_none; }
  bool is_publish_nan_as_none() const { return this->publish_nan_as_none_; }
  void set_wait_for_connection(bool wait_for_connection) { this->wait_for_connection_ = wait_for_connection; }

  // --- Test drivers, in place of the broker ---

  // The broker took the connection, in upstream's order: the callbacks run while the client
  // still reads as not connected, then it is connected and every child is due a resend.
  void connect_for_test(bool session_present = false);
  // The connection went, or never came: each disconnect callback hears the reason once.
  void drop_for_test(MQTTClientDisconnectReason reason) {
    this->connected_ = false;
    for (auto &callback : this->on_disconnect_)
      callback(reason);
  }
  // A message on `topic`, to every subscription that matches it.
  void deliver_for_test(const std::string &topic, const std::string &payload) {
    for (auto &subscription : this->subscriptions_) {
      if (topic_match(topic.c_str(), subscription.topic.c_str()))
        subscription.callback(topic, payload);
    }
  }
  // One loop pass of upstream's resend work: at most 8 children. Returns how many it ran.
  size_t process_resends_for_test();

  // --- Read-backs ---
  const MQTTCredentials &credentials() const { return this->credentials_; }
  const MQTTMessage &birth_message() const { return this->birth_message_; }
  const MQTTMessage &last_will() const { return this->last_will_; }
  const MQTTMessage &shutdown_message() const { return this->shutdown_message_; }
  const MQTTMessage &log_message() const { return this->log_message_; }
  bool enable_on_boot() const { return this->enable_on_boot_; }
  bool enabled() const { return this->enabled_; }
  uint32_t reboot_timeout() const { return this->reboot_timeout_; }
  const std::vector<MQTTSubscription> &subscriptions() const { return this->subscriptions_; }
  const std::vector<MQTTComponent *> &children() const { return this->children_; }
  size_t discovery_info_calls() const { return this->discovery_info_calls_; }
  size_t disable_discovery_calls() const { return this->disable_discovery_calls_; }
  size_t on_connect_callbacks() const { return this->on_connect_.size(); }
  size_t on_disconnect_callbacks() const { return this->on_disconnect_.size(); }

  int enable_calls{0};
  int disable_calls{0};
  std::vector<Published> published;
  // Makes every publish fail, as a full outbox does.
  bool fail_publishes{false};

 protected:
  void recalculate_availability_() {
    if (this->birth_message_.topic.empty() || this->birth_message_.topic != this->last_will_.topic) {
      this->availability_.topic = "";
      return;
    }
    this->availability_.topic = this->birth_message_.topic;
    this->availability_.payload_available = this->birth_message_.payload;
    this->availability_.payload_not_available = this->last_will_.payload;
  }

  MQTTCredentials credentials_{
      .address = "", .port = 1883, .username = "", .password = "", .client_id = "", .clean_session = false};
  MQTTMessage last_will_{};
  MQTTMessage birth_message_{};
  MQTTMessage shutdown_message_{};
  MQTTMessage log_message_{};
  Availability availability_{};
  // Upstream's defaults, before codegen sets them.
  MQTTDiscoveryInfo discovery_info_{
      .prefix = "homeassistant",
      .retain = true,
      .discover_ip = true,
      .clean = false,
      .unique_id_generator = MQTT_LEGACY_UNIQUE_ID_GENERATOR,
      .object_id_generator = MQTT_NONE_OBJECT_ID_GENERATOR,
  };
  std::string topic_prefix_{};
  int log_level_{ESPHOME_LOG_LEVEL};
  uint16_t keep_alive_s_{15};
  uint32_t reboot_timeout_{300000};
  bool enable_on_boot_{true};
  bool enabled_{false};
  bool connected_{false};
  bool publish_nan_as_none_{false};
  bool wait_for_connection_{false};
  size_t discovery_info_calls_{0};
  size_t disable_discovery_calls_{0};
  std::vector<MQTTSubscription> subscriptions_;
  std::vector<MQTTComponent *> children_;
  std::vector<mqtt_on_connect_callback_t> on_connect_;
  std::vector<mqtt_on_disconnect_callback_t> on_disconnect_;
};

}  // namespace esphome::mqtt

// The drivers above that walk the children need MQTTComponent whole.
#include "mqtt_component.h"

#endif  // USE_MQTT
