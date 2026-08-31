#include "modbus_server_group.h"
#include "esphome/core/log.h"

namespace esphome {
namespace modbus_server_group {

static const char *const TAG = "modbus_server_group";

/// Input register holding the number of discrete inputs.
static constexpr uint16_t INPUTS_COUNT_ADDRESS = 0x0200;
/// Input register holding the number of coils.
static constexpr uint16_t OUTPUTS_COUNT_ADDRESS = 0x0201;

void ModbusServerGroup::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Modbus Server Group...");

#ifdef USE_BINARY_SENSOR
  if (this->inputs_group_ != nullptr) {
    for (EntityBase *entity : this->inputs_group_->items()) {
      this->discrete_inputs_.push_back(static_cast<binary_sensor::BinarySensor *>(entity));
      ESP_LOGD(TAG, "  Added discrete input at 0x%04X for '%s'",
               static_cast<uint16_t>(this->inputs_start_address_ + this->discrete_inputs_.size() - 1),
               entity->get_name().c_str());
    }
    this->inputs_count_ = static_cast<uint16_t>(this->discrete_inputs_.size());
  }
#endif

#ifdef USE_SWITCH
  if (this->outputs_group_ != nullptr) {
    for (EntityBase *entity : this->outputs_group_->items()) {
      this->coils_.push_back(static_cast<switch_::Switch *>(entity));
      ESP_LOGD(TAG, "  Added coil at 0x%04X for '%s'",
               static_cast<uint16_t>(this->outputs_start_address_ + this->coils_.size() - 1),
               entity->get_name().c_str());
    }
    this->outputs_count_ = static_cast<uint16_t>(this->coils_.size());
  }
#endif

  ESP_LOGCONFIG(TAG, "Configured %u coil(s) at 0x%04X and %u discrete input(s) at 0x%04X", this->outputs_count_,
                this->outputs_start_address_, this->inputs_count_, this->inputs_start_address_);
}

int ModbusServerGroup::coil_index_(uint32_t address) const {
#ifdef USE_SWITCH
  if (address >= this->outputs_start_address_ &&
      address < static_cast<uint32_t>(this->outputs_start_address_) + this->coils_.size()) {
    return static_cast<int>(address - this->outputs_start_address_);
  }
#endif
  return -1;
}

int ModbusServerGroup::discrete_input_index_(uint32_t address) const {
#ifdef USE_BINARY_SENSOR
  if (address >= this->inputs_start_address_ &&
      address < static_cast<uint32_t>(this->inputs_start_address_) + this->discrete_inputs_.size()) {
    return static_cast<int>(address - this->inputs_start_address_);
  }
#endif
  return -1;
}

modbus::ResponseStatus ModbusServerGroup::on_read_coils(uint16_t start_address, modbus::MutablePackedBits bits) {
  ESP_LOGV(TAG, "Read coils for device 0x%02X. Start address: 0x%04X. Count: %u.", this->address_, start_address,
           bits.size());

  for (uint16_t i = 0; i < bits.size(); i++) {
    const uint32_t address = static_cast<uint32_t>(start_address) + i;
    const int index = this->coil_index_(address);
    if (index >= 0) {
#ifdef USE_SWITCH
      bits.set(i, this->coils_[index]->state);
#endif
      continue;
    }
    if (this->courtesy_response_.enabled && address <= this->courtesy_response_.coil_last_address) {
      bits.set(i, this->courtesy_response_.coil_value);
      continue;
    }
    ESP_LOGW(TAG, "No coil at 0x%04X and courtesy default not allowed. Sending exception response.",
             static_cast<uint16_t>(address));
    return modbus::ExceptionCode::ILLEGAL_DATA_ADDRESS;
  }
  return {};
}

modbus::ResponseStatus ModbusServerGroup::on_read_discrete_inputs(uint16_t start_address,
                                                                  modbus::MutablePackedBits bits) {
  ESP_LOGV(TAG, "Read discrete inputs for device 0x%02X. Start address: 0x%04X. Count: %u.", this->address_,
           start_address, bits.size());

  for (uint16_t i = 0; i < bits.size(); i++) {
    const uint32_t address = static_cast<uint32_t>(start_address) + i;
    const int index = this->discrete_input_index_(address);
    if (index >= 0) {
#ifdef USE_BINARY_SENSOR
      bits.set(i, this->discrete_inputs_[index]->state);
#endif
      continue;
    }
    if (this->courtesy_response_.enabled && address <= this->courtesy_response_.discrete_input_last_address) {
      bits.set(i, this->courtesy_response_.discrete_input_value);
      continue;
    }
    ESP_LOGW(TAG, "No discrete input at 0x%04X and courtesy default not allowed. Sending exception response.",
             static_cast<uint16_t>(address));
    return modbus::ExceptionCode::ILLEGAL_DATA_ADDRESS;
  }
  return {};
}

modbus::ResponseStatus ModbusServerGroup::on_write_coils(uint16_t start_address, modbus::PackedBits bits) {
  ESP_LOGV(TAG, "Write coils for device 0x%02X. Start address: 0x%04X. Count: %u.", this->address_, start_address,
           bits.size());

  // Pre-flight: every targeted coil must be mapped, so a rejected request never applies a partial write.
  for (uint16_t i = 0; i < bits.size(); i++) {
    const uint32_t address = static_cast<uint32_t>(start_address) + i;
    if (this->coil_index_(address) < 0) {
      // VERBOSE only: this handler also serves broadcasts, where not mapping an address is routine.
      ESP_LOGV(TAG, "No writable coil at 0x%04X; write request rejected before applying any coil.",
               static_cast<uint16_t>(address));
      return modbus::ExceptionCode::ILLEGAL_DATA_ADDRESS;
    }
  }

#ifdef USE_SWITCH
  for (uint16_t i = 0; i < bits.size(); i++) {
    const uint32_t address = static_cast<uint32_t>(start_address) + i;
    switch_::Switch *sw = this->coils_[this->coil_index_(address)];
    if (bits[i]) {
      sw->turn_on();
    } else {
      sw->turn_off();
    }
  }
#endif
  return {};
}

modbus::ResponseStatus ModbusServerGroup::on_read_input_registers(uint16_t start_address, uint16_t number_of_registers,
                                                                  modbus::RegisterValues &registers) {
  ESP_LOGV(TAG, "Read input registers for device 0x%02X. Start address: 0x%04X. Count: %u.", this->address_,
           start_address, number_of_registers);

  const uint32_t end_address = static_cast<uint32_t>(start_address) + number_of_registers;
  for (uint32_t address = start_address; address < end_address; address++) {
    if (address == INPUTS_COUNT_ADDRESS) {
      registers.push_back(this->inputs_count_);
      continue;
    }
    if (address == OUTPUTS_COUNT_ADDRESS) {
      registers.push_back(this->outputs_count_);
      continue;
    }
    if (this->courtesy_response_.enabled && address <= this->courtesy_response_.register_last_address) {
      registers.push_back(this->courtesy_response_.register_value);
      continue;
    }
    ESP_LOGW(TAG, "No input register at 0x%04X and courtesy default not allowed. Sending exception response.",
             static_cast<uint16_t>(address));
    return modbus::ExceptionCode::ILLEGAL_DATA_ADDRESS;
  }
  return {};
}

modbus::ResponseStatus ModbusServerGroup::on_read_holding_registers(uint16_t start_address,
                                                                    uint16_t number_of_registers,
                                                                    modbus::RegisterValues &registers) {
  // No holding registers are mapped: the entity counts live in the input register space (FC 0x04).
  // Only the courtesy default can answer here, matching the JetHome fork's behaviour with an empty
  // holding register table.
  if (!this->courtesy_response_.enabled) {
    return modbus::ExceptionCode::ILLEGAL_DATA_ADDRESS;
  }

  const uint32_t end_address = static_cast<uint32_t>(start_address) + number_of_registers;
  for (uint32_t address = start_address; address < end_address; address++) {
    if (address > this->courtesy_response_.register_last_address) {
      ESP_LOGW(TAG, "No holding register at 0x%04X and courtesy default not allowed. Sending exception response.",
               static_cast<uint16_t>(address));
      return modbus::ExceptionCode::ILLEGAL_DATA_ADDRESS;
    }
    registers.push_back(this->courtesy_response_.register_value);
  }
  return {};
}

void ModbusServerGroup::dump_config() {
  ESP_LOGCONFIG(TAG, "Modbus Server Group:");

  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup failed!");
    return;
  }

  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->address_);

  if (this->inputs_group_ != nullptr) {
    ESP_LOGCONFIG(TAG,
                  "  Discrete Inputs:\n"
                  "    Group: %s\n"
                  "    Start Address: 0x%04X\n"
                  "    Count: %u",
                  this->inputs_group_->get_name().c_str(), this->inputs_start_address_, this->inputs_count_);
  }

  if (this->outputs_group_ != nullptr) {
    ESP_LOGCONFIG(TAG,
                  "  Coils:\n"
                  "    Group: %s\n"
                  "    Start Address: 0x%04X\n"
                  "    Count: %u",
                  this->outputs_group_->get_name().c_str(), this->outputs_start_address_, this->outputs_count_);
  }

  ESP_LOGCONFIG(TAG,
                "  Input Registers:\n"
                "    0x%04X (inputs_count): %u\n"
                "    0x%04X (outputs_count): %u\n"
                "  Courtesy Response:\n"
                "    Enabled: %s",
                INPUTS_COUNT_ADDRESS, this->inputs_count_, OUTPUTS_COUNT_ADDRESS, this->outputs_count_,
                YESNO(this->courtesy_response_.enabled));
}

}  // namespace modbus_server_group
}  // namespace esphome
