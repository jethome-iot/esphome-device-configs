#pragma once

#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/components/modbus_controller/modbus_controller.h"
#include "esphome/components/groups/groups.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

namespace esphome {
namespace modbus_server_group {

/**
 * @brief Component for automatically configuring Modbus server from ESPHome groups
 * 
 * This component simplifies Modbus server configuration by automatically creating
 * server coils and discrete inputs based on group membership. It eliminates the
 * need for manual lambda configuration for each entity.
 * 
 * Features:
 * - Automatically registers switches from output group as Modbus coils (read/write)
 * - Automatically registers binary sensors from input group as discrete inputs (read-only)
 * - Provides holding registers at 0x0200 (input count) and 0x0201 (output count)
 * - Dynamic configuration based on actual entities in groups
 */
class ModbusServerGroup : public Component {
 public:
  ModbusServerGroup() = default;

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /**
   * @brief Set the Modbus controller to configure
   * @param controller Pointer to ModbusController instance
   */
  void set_modbus_controller(modbus_controller::ModbusController *controller) { 
    this->modbus_controller_ = controller; 
  }

  /**
   * @brief Set the group containing input entities (binary sensors)
   * @param group Pointer to Group containing binary sensors
   */
  void set_inputs_group(groups::Group *group) { this->inputs_group_ = group; }

  /**
   * @brief Set starting address for discrete inputs
   * @param address Starting Modbus address (default: 0xA100)
   */
  void set_inputs_start_address(uint16_t address) { this->inputs_start_address_ = address; }

  /**
   * @brief Set the group containing output entities (switches)
   * @param group Pointer to Group containing switches
   */
  void set_outputs_group(groups::Group *group) { this->outputs_group_ = group; }

  /**
   * @brief Set starting address for coils
   * @param address Starting Modbus address (default: 0xA000)
   */
  void set_outputs_start_address(uint16_t address) { this->outputs_start_address_ = address; }

 protected:
  modbus_controller::ModbusController *modbus_controller_{nullptr};
  groups::Group *inputs_group_{nullptr};
  groups::Group *outputs_group_{nullptr};
  uint16_t inputs_start_address_{0xA100};
  uint16_t outputs_start_address_{0xA000};
  
  uint16_t inputs_count_{0};
  uint16_t outputs_count_{0};

  /**
   * @brief Setup discrete inputs for binary sensors in the inputs group
   */
  void setup_discrete_inputs_();

  /**
   * @brief Setup coils for switches in the outputs group
   */
  void setup_coils_();

  /**
   * @brief Setup holding registers for count information
   */
  void setup_count_registers_();
};

}  // namespace modbus_server_group
}  // namespace esphome


