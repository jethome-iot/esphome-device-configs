#include "hold_aware_switch.h"

#ifdef USE_SWITCH

#include "esphome/components/mqtt/mqtt_const.h"
#include "esphome/components/switch_hold/switch_hold.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::mqtt_config {

static const char *const TAG = "mqtt_config.switch";

// Upstream's MQTTSwitchComponent::setup(), with the command going through handle_command().
void HoldAwareSwitch::setup() {
  this->subscribe(this->get_command_topic_(),
                  [this](const std::string & /*topic*/, const std::string &payload) { this->handle_command(payload); });
  this->switch_->add_on_state_callback(
      [this](bool state) { this->defer("send", [this, state]() { this->publish_state(state); }); });
}

void HoldAwareSwitch::dump_config() {
  ESP_LOGCONFIG(TAG, "MQTT Switch '%s', refusing what would move it while a thermostat holds it",
                this->switch_->get_name().c_str());
}

const char *HoldAwareSwitch::component_type() const { return "switch"; }

void HoldAwareSwitch::send_discovery(JsonObject root, mqtt::SendDiscoveryConfig & /*config*/) {
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks) false positive with ArduinoJson, as upstream's
  if (this->switch_->assumed_state())
    root[mqtt::MQTT_OPTIMISTIC] = true;
}

bool HoldAwareSwitch::send_initial_state() { return this->publish_state(this->switch_->state); }

bool HoldAwareSwitch::publish_state(bool state) {
  return this->publish(this->get_state_topic_(), state ? "ON" : "OFF");
}

void HoldAwareSwitch::handle_command(const std::string &payload) {
  const ParseOnOffState parsed = parse_on_off(payload.c_str());
  if (parsed == PARSE_NONE) {
    ESP_LOGW(TAG, "'%s': Received unknown status payload: %s", this->switch_->get_name().c_str(), payload.c_str());
    this->status_momentary_warning("state", 5000);
    return;
  }
  // The state it already has is taken, as a Modbus write of it is.
  const bool target = parsed == PARSE_TOGGLE ? !this->switch_->state : parsed == PARSE_ON;
  if (target != this->switch_->state) {
    if (const std::string holder = switch_hold::holder(this->switch_); !holder.empty()) {
      ESP_LOGW(TAG, "'%s' is driven by thermostat '%s'; the MQTT command is refused", this->switch_->get_name().c_str(),
               holder.c_str());
      // A client that assumed the change hears that it did not happen.
      this->publish_state(this->switch_->state);
      return;
    }
  }
  if (parsed == PARSE_ON) {
    this->switch_->turn_on();
  } else if (parsed == PARSE_OFF) {
    this->switch_->turn_off();
  } else {
    this->switch_->toggle();
  }
}

}  // namespace esphome::mqtt_config

#endif  // USE_SWITCH
