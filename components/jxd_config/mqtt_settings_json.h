#pragma once

#include "esphome/core/defines.h"
#ifdef JXD_CONFIG_MQTT

#include <string>
#include <vector>
#include "esphome/components/config_json/settings_base_json.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

namespace esphome::jxd_config {

// Broker, credentials, client_id, topic_prefix and discovery. With enabled=false or no broker
// the client is disabled; otherwise the stored values replace the compiled ones.
class MqttSettingsJson : public config_json::SettingsBaseJson {
 public:
  static constexpr const char *TAG = "jxd_config.mqtt";
  static constexpr const char *NAME = "mqtt";

  const char *get_key() override { return NAME; }

  // Before load: what discovery on means is whatever the YAML compiled in.
  void init() override {
#ifdef USE_MQTT
    if (mqtt::global_mqtt_client != nullptr) {
      this->compiled_discovery_ = mqtt::global_mqtt_client->get_discovery_info();
      if (this->compiled_discovery_.prefix.empty())
        this->compiled_discovery_.prefix = "homeassistant";
    }
#endif
  }

  bool parse_json(JsonObject root, uint32_t version) override {
    this->enabled_ = root["enabled"] | false;
    this->broker_ = root["broker"] | std::string("");
    this->port_ = root["port"] | uint16_t(1883);
    this->username_ = root["username"] | std::string("");
    this->password_ = root["password"] | std::string("");
    this->client_id_ = root["client_id"] | std::string("");
    this->topic_prefix_ = root["topic_prefix"] | std::string("");
    this->discovery_ = root["discovery"] | true;
    ESP_LOGI(TAG, "Loaded MQTT settings: enabled=%s broker=%s port=%u", YESNO(this->enabled_),
             this->broker_.c_str(), this->port_);
    return true;
  }

  void write_json(JsonObject root, uint32_t version) override {
    root["enabled"] = this->enabled_;
    root["broker"] = this->broker_;
    root["port"] = this->port_;
    root["username"] = this->username_;
    root["password"] = this->password_;
    root["client_id"] = this->client_id_;
    root["topic_prefix"] = this->topic_prefix_;
    root["discovery"] = this->discovery_;
  }

  void apply() override {
#ifdef USE_MQTT
    auto *client = mqtt::global_mqtt_client;
    if (client == nullptr)
      return;

    this->apply_discovery();

    if (!this->enabled_ || this->broker_.empty()) {
      ESP_LOGD(TAG, "MQTT not enabled or broker not set, disabling client");
      client->disable();
      return;
    }

    // A running client reconnects with the new values and re-announces discovery.
    if (client->is_connected())
      client->disable();

    client->set_broker_address(this->broker_);
    client->set_broker_port(this->port_);
    client->set_username(this->username_);
    client->set_password(this->password_);
    if (!this->client_id_.empty())
      client->set_client_id(this->client_id_);

    std::string prefix = this->topic_prefix_;
    if (prefix.empty()) {
      char buf[ESPHOME_DEVICE_NAME_MAX_LEN + 1];
      prefix = str_sanitize_to(buf, App.get_name().c_str());
    }
    client->set_topic_prefix(prefix, "");

    ESP_LOGI(TAG, "Applied MQTT settings: broker=%s port=%u prefix=%s", this->broker_.c_str(), this->port_,
             prefix.c_str());
    client->enable();
#endif
  }

  // Discovery alone, so a save can apply it without a reconnect.
  void apply_discovery() {
#ifdef USE_MQTT
    auto *client = mqtt::global_mqtt_client;
    if (client == nullptr)
      return;
    if (this->discovery_) {
      const auto &d = this->compiled_discovery_;
      client->set_discovery_info(std::string(d.prefix), d.unique_id_generator, d.object_id_generator, d.retain,
                                 d.discover_ip, d.clean);
    } else {
      client->disable_discovery();
    }
#endif
  }

  // Just before the client's AFTER_WIFI setup.
  static constexpr float APPLY_PRIORITY = setup_priority::AFTER_WIFI + 1.0f;

  size_t size() override { return this->enabled_ ? 1 : 0; }

  void reset() override {
    this->enabled_ = false;
    this->broker_.clear();
    this->port_ = 1883;
    this->username_.clear();
    this->password_.clear();
    this->client_id_.clear();
    this->topic_prefix_.clear();
    this->discovery_ = true;
    this->mark_dirty();
    ESP_LOGI(TAG, "Reset MQTT settings to defaults");
  }

  void set_enabled(bool enabled) { this->set_(this->enabled_, enabled); }
  void set_broker(const std::string &broker) { this->set_(this->broker_, broker); }
  void set_port(uint16_t port) { this->set_(this->port_, port); }
  void set_username(const std::string &username) { this->set_(this->username_, username); }
  void set_password(const std::string &password) { this->set_(this->password_, password); }
  void set_client_id(const std::string &client_id) { this->set_(this->client_id_, client_id); }
  void set_topic_prefix(const std::string &prefix) { this->set_(this->topic_prefix_, prefix); }
  void set_discovery(bool discovery) { this->set_(this->discovery_, discovery); }

  bool get_enabled() const { return this->enabled_; }
  const std::string &get_broker() const { return this->broker_; }
  uint16_t get_port() const { return this->port_; }
  const std::string &get_username() const { return this->username_; }
  const std::string &get_password() const { return this->password_; }
  const std::string &get_client_id() const { return this->client_id_; }
  const std::string &get_topic_prefix() const { return this->topic_prefix_; }
  bool get_discovery() const { return this->discovery_; }

 protected:
  template<typename T> void set_(T &field, const T &value) {
    if (field != value) {
      field = value;
      this->mark_dirty();
    }
  }

  bool enabled_{false};
  std::string broker_;
  uint16_t port_{1883};
  std::string username_;
  std::string password_;
  std::string client_id_;
  std::string topic_prefix_;
  bool discovery_{true};
#ifdef USE_MQTT
  mqtt::MQTTDiscoveryInfo compiled_discovery_{};
#endif
};

}  // namespace esphome::jxd_config

#endif  // JXD_CONFIG_MQTT
