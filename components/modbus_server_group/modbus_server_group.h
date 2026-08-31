#pragma once

#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/components/modbus/modbus.h"
#include "esphome/components/groups/groups.h"

#include <vector>

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

namespace esphome {
namespace modbus_server_group {

/// Values answered for addresses this device does not map. Mirrors the behaviour the JetHome
/// modbus_controller fork provided through `server_courtesy_response:`: a read of an unmapped
/// address up to `*_last_address` answers with the configured default instead of an exception,
/// which keeps scanning Modbus masters happy.
struct CourtesyResponse {
  bool enabled{false};
  uint16_t register_last_address{0xFFFF};
  uint16_t register_value{0};
  uint16_t coil_last_address{0xFFFF};
  bool coil_value{false};
  uint16_t discrete_input_last_address{0xFFFF};
  bool discrete_input_value{false};
};

/**
 * @brief Modbus RTU server device driven by ESPHome groups.
 *
 * Attaches directly to a `modbus:` hub configured with `role: server` and answers the
 * coil/discrete-input/register function codes itself, so no per-entity lambdas are needed.
 *
 * Unlike the upstream `modbus_server` component -- which serves FC 0x01 and FC 0x02 from a single
 * shared bit table and therefore cannot have a coil and a discrete input at the same address --
 * this component keeps two independent bit spaces by overriding `on_read_coils()` and
 * `on_read_discrete_inputs()` separately. That preserves the historical JXD register map where
 * relays and digital inputs both start at 0xA000.
 *
 * Register map:
 * - FC 0x01 / 0x05 / 0x0F: coils at `outputs_start_address` + N -> switches in the outputs group
 * - FC 0x02: discrete inputs at `inputs_start_address` + N -> binary sensors in the inputs group
 * - FC 0x04: input register 0x0200 = number of inputs, 0x0201 = number of outputs
 */
class ModbusServerGroup : public Component, public modbus::ModbusServerDevice {
 public:
  ModbusServerGroup() = default;

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_courtesy_response(const CourtesyResponse &courtesy_response) {
    this->courtesy_response_ = courtesy_response;
  }

  /// @brief Set the group containing input entities (binary sensors)
  void set_inputs_group(groups::Group *group) { this->inputs_group_ = group; }
  /// @brief Set starting address for discrete inputs
  void set_inputs_start_address(uint16_t address) { this->inputs_start_address_ = address; }
  /// @brief Set the group containing output entities (switches)
  void set_outputs_group(groups::Group *group) { this->outputs_group_ = group; }
  /// @brief Set starting address for coils
  void set_outputs_start_address(uint16_t address) { this->outputs_start_address_ = address; }

  /// FC 0x01 - served from the switches in the outputs group.
  modbus::ResponseStatus on_read_coils(uint16_t start_address, modbus::MutablePackedBits bits) override;
  /// FC 0x02 - served from the binary sensors in the inputs group, in a separate address space.
  modbus::ResponseStatus on_read_discrete_inputs(uint16_t start_address, modbus::MutablePackedBits bits) override;
  /// FC 0x05 / 0x0F - drives the switches in the outputs group.
  modbus::ResponseStatus on_write_coils(uint16_t start_address, modbus::PackedBits bits) override;
  /// FC 0x04 - the entity count registers.
  modbus::ResponseStatus on_read_input_registers(uint16_t start_address, uint16_t number_of_registers,
                                                 modbus::RegisterValues &registers) override;
  /// FC 0x03 - no holding registers are mapped; answers only from the courtesy default.
  modbus::ResponseStatus on_read_holding_registers(uint16_t start_address, uint16_t number_of_registers,
                                                   modbus::RegisterValues &registers) override;

 protected:
  /// Index of `address` inside the coil range, or -1 when it is outside it.
  int coil_index_(uint32_t address) const;
  /// Index of `address` inside the discrete input range, or -1 when it is outside it.
  int discrete_input_index_(uint32_t address) const;

  groups::Group *inputs_group_{nullptr};
  groups::Group *outputs_group_{nullptr};
  uint16_t inputs_start_address_{0xA100};
  uint16_t outputs_start_address_{0xA000};

  uint16_t inputs_count_{0};
  uint16_t outputs_count_{0};

  CourtesyResponse courtesy_response_{};

#ifdef USE_BINARY_SENSOR
  std::vector<binary_sensor::BinarySensor *> discrete_inputs_{};
#endif
#ifdef USE_SWITCH
  std::vector<switch_::Switch *> coils_{};
#endif
};

}  // namespace modbus_server_group
}  // namespace esphome
