#pragma once

// Host stand-in for upstream's MQTTComponent and the entity classes codegen creates one of per
// entity once `mqtt:` is in the config. They keep upstream's setters, its internal rule, the
// resend flag and a discovery publish whose payload says only whether it was a clean; a test
// reads what went out from the client's `published`.

#include "esphome/core/defines.h"

#ifdef USE_MQTT

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"
#include "esphome/core/optional.h"
#include "mqtt_client.h"

#ifdef USE_ALARM_CONTROL_PANEL
#include "esphome/components/alarm_control_panel/alarm_control_panel.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_CLIMATE
#include "esphome/components/climate/climate.h"
#endif
#ifdef USE_COVER
#include "esphome/components/cover/cover.h"
#endif
#ifdef USE_DATETIME_DATE
#include "esphome/components/datetime/date_entity.h"
#endif
#ifdef USE_DATETIME_DATETIME
#include "esphome/components/datetime/datetime_entity.h"
#endif
#ifdef USE_DATETIME_TIME
#include "esphome/components/datetime/time_entity.h"
#endif
#ifdef USE_EVENT
#include "esphome/components/event/event.h"
#endif
#ifdef USE_FAN
#include "esphome/components/fan/fan.h"
#endif
#ifdef USE_LIGHT
#include "esphome/components/light/light_state.h"
#endif
#ifdef USE_LOCK
#include "esphome/components/lock/lock.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_TEXT
#include "esphome/components/text/text.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_UPDATE
#include "esphome/components/update/update_entity.h"
#endif
#ifdef USE_VALVE
#include "esphome/components/valve/valve.h"
#endif

namespace esphome::mqtt {

class MQTTComponent : public Component {
 public:
  // As upstream: the internal flag is decided once, here, and an internal component neither
  // sets up nor registers with the client.
  void call_setup() override {
    this->is_internal_ = this->compute_is_internal_();
    if (this->is_internal_)
      return;
    this->setup();
    global_mqtt_client->register_mqtt_component(this);
  }

  virtual const char *component_type() const = 0;
  // Upstream publishes the entity's state; the stand-ins have none of their own to send.
  virtual bool send_initial_state() { return true; }

  bool is_internal() const { return this->is_internal_; }

  void set_qos(uint8_t qos) { this->qos_ = qos; }
  uint8_t get_qos() const { return this->qos_; }
  void set_retain(bool retain) { this->retain_ = retain; }
  bool get_retain() const { return this->retain_; }
  void disable_discovery() { this->discovery_enabled_ = false; }
  bool is_discovery_enabled() const { return this->discovery_enabled_ && global_mqtt_client->is_discovery_enabled(); }
  void set_subscribe_qos(uint8_t qos) { this->subscribe_qos_ = qos; }
  template<typename T> void set_custom_state_topic(T &&topic) { this->custom_state_topic_ = std::string(topic); }
  template<typename T> void set_custom_command_topic(T &&topic) { this->custom_command_topic_ = std::string(topic); }
  void set_command_retain(bool command_retain) { this->command_retain_ = command_retain; }
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }
  void set_availability(std::string topic, std::string payload_available, std::string payload_not_available) {
    this->availability_ =
        Availability{std::move(topic), std::move(payload_available), std::move(payload_not_available)};
  }
  void disable_availability() { this->set_availability("", "", ""); }

  void schedule_resend_state() { this->resend_state_ = true; }
  bool is_resend_pending() const { return this->resend_state_; }
  // As upstream: discovery first (an empty retained payload in clean mode), then the state; a
  // failed publish puts the resend back.
  void process_resend() {
    if (!this->resend_state_)
      return;
    this->resend_state_ = false;
    if (this->is_discovery_enabled() && !this->send_discovery_())
      this->schedule_resend_state();
    if (!this->send_initial_state())
      this->schedule_resend_state();
  }

  // Read-backs.
  const optional<std::string> &custom_state_topic() const { return this->custom_state_topic_; }
  const optional<std::string> &custom_command_topic() const { return this->custom_command_topic_; }
  // `<prefix>/<type>/<node>/<object_id>/config`, as upstream builds it.
  std::string discovery_topic() const {
    char node[ESPHOME_DEVICE_NAME_MAX_LEN + 1];
    str_sanitize_to(node, App.get_name().c_str());
    return global_mqtt_client->get_discovery_info().prefix + "/" + this->component_type() + "/" + node + "/" +
           this->object_id_() + "/config";
  }
  // `<topic prefix>/<type>/<object_id>/state` unless a custom one is set.
  std::string state_topic() const {
    if (this->custom_state_topic_.has_value())
      return *this->custom_state_topic_;
    return global_mqtt_client->get_topic_prefix() + "/" + this->component_type() + "/" + this->object_id_() + "/state";
  }

 protected:
  virtual const EntityBase *get_entity() const = 0;

  std::string object_id_() const {
    char buf[OBJECT_ID_MAX_LEN];
    const StringRef id = this->get_entity()->get_object_id_to(buf);
    return std::string(id.c_str(), id.size());
  }

  bool send_discovery_() {
    const MQTTDiscoveryInfo &info = global_mqtt_client->get_discovery_info();
    const std::string topic = this->discovery_topic();
    if (info.clean)
      return global_mqtt_client->publish(topic.c_str(), "", 0, this->qos_, true);
    return global_mqtt_client->publish(topic.c_str(), "{}", 2, this->qos_, info.retain);
  }

  // Upstream's compute_is_internal_: an empty custom topic hides it, a set one shows it, and
  // otherwise an empty prefix or the entity's own flag decides.
  bool compute_is_internal_() {
    if (this->custom_state_topic_.has_value())
      return this->custom_state_topic_->empty();
    if (this->custom_command_topic_.has_value())
      return this->custom_command_topic_->empty();
    if (global_mqtt_client->get_topic_prefix().empty())
      return true;
    return this->get_entity()->is_internal();
  }

  optional<std::string> custom_state_topic_{};
  optional<std::string> custom_command_topic_{};
  optional<Availability> availability_{};
  uint8_t qos_{0};
  uint8_t subscribe_qos_{0};
  bool command_retain_{false};
  bool retain_{true};
  bool discovery_enabled_{true};
  bool resend_state_{false};
  bool is_internal_{false};
};

// One per entity domain: the entity it serves and its domain name. The per-domain topic
// setters of climate and cover (custom action, position, tilt topics) are not stood in.
template<typename E> class MQTTEntityComponent : public MQTTComponent {
 public:
  explicit MQTTEntityComponent(E *entity) : entity_(entity) {}
  E *entity() const { return this->entity_; }

 protected:
  const EntityBase *get_entity() const override { return this->entity_; }
  E *entity_;
};

#define MQTT_STAND_IN_(class_name, entity_type, type_str) \
  class class_name final : public MQTTEntityComponent<entity_type> { \
   public: \
    using MQTTEntityComponent::MQTTEntityComponent; \
    const char *component_type() const override { return type_str; } \
  };

#ifdef USE_ALARM_CONTROL_PANEL
MQTT_STAND_IN_(MQTTAlarmControlPanelComponent, alarm_control_panel::AlarmControlPanel, "alarm_control_panel")
#endif
#ifdef USE_BINARY_SENSOR
MQTT_STAND_IN_(MQTTBinarySensorComponent, binary_sensor::BinarySensor, "binary_sensor")
#endif
#ifdef USE_BUTTON
MQTT_STAND_IN_(MQTTButtonComponent, button::Button, "button")
#endif
#ifdef USE_CLIMATE
MQTT_STAND_IN_(MQTTClimateComponent, climate::Climate, "climate")
#endif
#ifdef USE_COVER
MQTT_STAND_IN_(MQTTCoverComponent, cover::Cover, "cover")
#endif
#ifdef USE_DATETIME_DATE
MQTT_STAND_IN_(MQTTDateComponent, datetime::DateEntity, "date")
#endif
#ifdef USE_DATETIME_DATETIME
MQTT_STAND_IN_(MQTTDateTimeComponent, datetime::DateTimeEntity, "datetime")
#endif
#ifdef USE_DATETIME_TIME
MQTT_STAND_IN_(MQTTTimeComponent, datetime::TimeEntity, "time")
#endif
#ifdef USE_EVENT
MQTT_STAND_IN_(MQTTEventComponent, event::Event, "event")
#endif
#ifdef USE_FAN
MQTT_STAND_IN_(MQTTFanComponent, fan::Fan, "fan")
#endif
#ifdef USE_LIGHT
MQTT_STAND_IN_(MQTTJSONLightComponent, light::LightState, "light")
#endif
#ifdef USE_LOCK
MQTT_STAND_IN_(MQTTLockComponent, lock::Lock, "lock")
#endif
#ifdef USE_NUMBER
MQTT_STAND_IN_(MQTTNumberComponent, number::Number, "number")
#endif
#ifdef USE_SELECT
MQTT_STAND_IN_(MQTTSelectComponent, select::Select, "select")
#endif
#ifdef USE_SWITCH
MQTT_STAND_IN_(MQTTSwitchComponent, switch_::Switch, "switch")
#endif
#ifdef USE_TEXT
MQTT_STAND_IN_(MQTTTextComponent, text::Text, "text")
#endif
#ifdef USE_TEXT_SENSOR
MQTT_STAND_IN_(MQTTTextSensor, text_sensor::TextSensor, "sensor")
#endif
#ifdef USE_UPDATE
MQTT_STAND_IN_(MQTTUpdateComponent, update::UpdateEntity, "update")
#endif
#ifdef USE_VALVE
MQTT_STAND_IN_(MQTTValveComponent, valve::Valve, "valve")
#endif

#undef MQTT_STAND_IN_

#ifdef USE_SENSOR
// Upstream's setup subscribes to the sensor and publishes every state, which is what a test of
// a component that hands sensors to MQTT has to see.
class MQTTSensorComponent final : public MQTTEntityComponent<sensor::Sensor> {
 public:
  using MQTTEntityComponent::MQTTEntityComponent;
  const char *component_type() const override { return "sensor"; }
  void setup() override {
    this->entity_->add_on_state_callback([this](float state) { this->publish_state(state); });
  }
  bool send_initial_state() override {
    return !this->entity_->has_state() || this->publish_state(this->entity_->state);
  }
  bool publish_state(float value) {
    char buf[VALUE_ACCURACY_MAX_LEN];
    const size_t len = value_accuracy_to_buf(buf, value, this->entity_->get_accuracy_decimals());
    return global_mqtt_client->publish(this->state_topic(), std::string(buf, len), this->qos_, this->retain_);
  }
  void set_expire_after(uint32_t expire_after) { this->expire_after_ = expire_after; }
  void disable_expire_after() { this->expire_after_ = 0; }

 protected:
  uint32_t expire_after_{0};
};
#endif

// Upstream calls the callbacks from its backend while its state is still CONNECTING;
// check_connected() then sets CONNECTED and schedules the resends in one go.
inline void MQTTClientComponent::take_connection_for_test() {
  this->connected_ = true;
  for (const MQTTSubscription &subscription : this->subscriptions_)
    this->sent_subscribes.push_back(subscription.topic);
  for (MQTTComponent *child : this->children_)
    child->schedule_resend_state();
}

inline size_t MQTTClientComponent::process_resends_for_test() {
  size_t count = 0;
  for (MQTTComponent *child : this->children_) {
    if (!child->is_resend_pending())
      continue;
    child->process_resend();
    if (++count >= 8)
      break;
  }
  return count;
}

}  // namespace esphome::mqtt

#endif  // USE_MQTT
