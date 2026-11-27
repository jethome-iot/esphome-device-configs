#pragma once

#include "esphome/core/defines.h"

#ifdef USE_SWITCH

#include <string>
#include "esphome/components/json/json_util.h"
#include "esphome/components/mqtt/mqtt_component.h"
#include "esphome/components/switch/switch.h"

namespace esphome::mqtt_config {

// A switch's MQTT component in place of upstream's MQTTSwitchComponent, which is final: the same
// topics, discovery and state, but a command that would move a switch a running thermostat holds
// is refused, as Modbus and the rules refuse it, and the state topic is told the state it kept.
class HoldAwareSwitch : public mqtt::MQTTComponent {
 public:
  explicit HoldAwareSwitch(switch_::Switch *a_switch) : switch_(a_switch) {}

  void setup() override;
  void dump_config() override;
  void send_discovery(JsonObject root, mqtt::SendDiscoveryConfig &config) override;
  bool send_initial_state() override;

  // A message on the command topic: ON, OFF or TOGGLE in any case, as upstream parses it.
  void handle_command(const std::string &payload);
  bool publish_state(bool state);

 protected:
  const char *component_type() const override;
  const EntityBase *get_entity() const override { return this->switch_; }

  switch_::Switch *switch_;
};

}  // namespace esphome::mqtt_config

#endif  // USE_SWITCH
